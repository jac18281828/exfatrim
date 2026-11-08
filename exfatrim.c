/*
 * exfatrim — TRIM the free space of an exFAT volume on macOS.
 *
 * macOS never sends TRIM to exFAT volumes, so an SSD formatted exFAT never
 * learns which blocks deleted files left behind. This tool:
 *
 *   1. unmounts the volume,
 *   2. checks it with Apple's fsck_exfat (refuses to touch a damaged volume),
 *   3. locks the raw device, reads exFAT's allocation bitmap and cross-checks it,
 *   4. with -v, samples free clusters that hold data,
 *   5. sends DKIOCUNMAP for the free clusters only,
 *   6. with -v, re-reads the sample: a trimmed cluster reads as zeros,
 *   7. mounts the volume again.
 *
 * Invariant: no byte outside a cluster the bitmap marks free, read on the
 * unmounted volume, is ever unmapped.
 *
 * Build:  make            (clang -O2 -Wall -Wextra -Werror exfatrim.c)
 * Use:    sudo exfatrim                    (the one mounted exFAT volume)
 *         sudo exfatrim "My SSD"           (by name, mount point or diskNsM)
 *         sudo exfatrim -n "My SSD"        (dry run: report only, change nothing)
 *         sudo exfatrim -v "My SSD"        (trim, then verify the drive dropped the data)
 *
 * Exit status: 0 success, 1 refused or error, 2 usage, 3 trim not confirmed
 * (-v), 4 trimmed but the volume did not mount again.
 *
 * Test hooks (environment): EXFATRIM_TEST_BLOCK raises the alignment unit to
 * a power of two at or above the device block size; EXFATRIM_TEST_SKIP_FSCK=1
 * skips the fsck_exfat gate.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_EXTENT_BYTES (256ULL << 20)
#define EXTENTS_PER_CALL 64
#define SAMPLE_TARGET 299
#define SAMPLE_MAX_DRAWS 3000
#define SAMPLE_READ_BYTES (1u << 20)
#define SEEN_SLOTS 8192
#define CONFIDENCE_TAIL 0.05

#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_UNCONFIRMED 3
#define EXIT_NOT_REMOUNTED 4

static const char *g_prog = "exfatrim";
static char g_name[256];
static char g_bsd[64];
static int g_owe_remount;
static int g_tty;
static volatile sig_atomic_t g_stop;

static int g_fd = -1;
static uint32_t g_blk;
static uint32_t g_align;
static uint64_t g_devsize;
static uint64_t g_trimmed;

static void status(const char *fmt, ...)
{
    if (!g_tty) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("\r\033[K", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void clear_status(void)
{
    if (g_tty) { fputs("\r\033[K", stderr); fflush(stderr); }
}

static int run_quiet(const char *path, char *const argv[])
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    pid_t pid;
    int rc = posix_spawn(&pid, path, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return -1;
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void remount(void)
{
    if (!g_owe_remount) return;
    char dev[80];
    snprintf(dev, sizeof dev, "/dev/%s", g_bsd);
    char *argv[] = {"diskutil", "mount", dev, NULL};
    if (run_quiet("/usr/sbin/diskutil", argv) == 0) g_owe_remount = 0;
}

static void die(const char *fmt, ...)
{
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    clear_status();
    remount();
    va_list ap;
    va_start(ap, fmt);
    fputs("✗ ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (g_owe_remount) {
        fprintf(stderr, "  %s is still unmounted: run  diskutil mount %s  or replug it.\n", g_name, g_bsd);
        exit(EXIT_NOT_REMOUNTED);
    }
    exit(EXIT_REFUSED);
}

static const char *nothing_trimmed(void)
{
    return g_trimmed ? "" : " Nothing was trimmed.";
}

static const char *human(uint64_t b, char *buf, size_t n)
{
    static const char *unit[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    if (b < 1000) { snprintf(buf, n, "%llu bytes", (unsigned long long)b); return buf; }
    double v = (double)b;
    int i = 0;
    while (v >= 1000 && i < 6) { v /= 1000; i++; }
    if (v >= 999.5 && i < 6) { v /= 1000; i++; }
    snprintf(buf, n, v < 9.95 ? "%.1f %s" : "%.0f %s", v, unit[i]);
    return buf;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void rd(uint64_t off, void *dst, size_t len)
{
    if (len == 0) return;
    if (off + len < off || off + len > g_devsize)
        die("Tried to read past the end of %s.%s", g_name, nothing_trimmed());
    uint64_t s = off / g_blk * g_blk;
    uint64_t e = (off + len + g_blk - 1) / g_blk * g_blk;
    size_t n = (size_t)(e - s);
    void *buf;
    if (posix_memalign(&buf, 4096, n)) die("Out of memory.");
    for (size_t done = 0; done < n;) {
        size_t chunk = n - done > (1u << 20) ? (1u << 20) : n - done;
        ssize_t r = pread(g_fd, (char *)buf + done, chunk, (off_t)(s + done));
        if (r <= 0) die("Read error on %s: %s.%s", g_name, r < 0 ? strerror(errno) : "short read", nothing_trimmed());
        done += (size_t)r;
    }
    memcpy(dst, (char *)buf + (off - s), len);
    free(buf);
}

struct fs {
    uint32_t bps;
    uint64_t clus;
    uint64_t fat_off;
    uint64_t fat_len;
    uint64_t heap_off;
    uint32_t nclus;
    uint32_t root;
    uint16_t flags;
};

static int valid_clus(const struct fs *f, uint32_t c) { return c >= 2 && c - 2 < f->nclus; }
static uint64_t clus_off(const struct fs *f, uint32_t c) { return f->heap_off + (uint64_t)(c - 2) * f->clus; }

static uint32_t fat_next(const struct fs *f, uint32_t c)
{
    uint8_t b[4];
    rd(f->fat_off + (uint64_t)c * 4, b, 4);
    return le32(b);
}

static uint32_t *chain(const struct fs *f, uint32_t first, uint64_t want, uint32_t *count, const char *what)
{
    if (want > f->nclus)
        die("Safety check failed: the %s claims more clusters than the volume has. Nothing was trimmed.", what);
    uint8_t *seen = calloc(((uint64_t)f->nclus + 7) / 8, 1);
    if (!seen) die("Out of memory.");
    uint64_t cap = want ? want : 16, n = 0;
    uint32_t *v = malloc(cap * sizeof *v);
    if (!v) die("Out of memory.");
    uint32_t c = first;
    for (;;) {
        if (!valid_clus(f, c))
            die("Safety check failed: the %s points at cluster %u, outside the volume. Nothing was trimmed.", what, c);
        if (seen[(c - 2) >> 3] >> ((c - 2) & 7) & 1)
            die("Safety check failed: the %s's cluster chain loops. Nothing was trimmed.", what);
        seen[(c - 2) >> 3] |= (uint8_t)(1u << ((c - 2) & 7));
        if (n == cap) {
            cap *= 2;
            if (cap > f->nclus) cap = f->nclus;
            if (!(v = realloc(v, cap * sizeof *v))) die("Out of memory.");
        }
        v[n++] = c;
        if (want && n == want) break;
        uint32_t next = fat_next(f, c);
        if (next == 0xFFFFFFFFu) {
            if (want) die("Safety check failed: the %s's cluster chain ends early. Nothing was trimmed.", what);
            break;
        }
        c = next;
    }
    free(seen);
    *count = (uint32_t)n;
    return v;
}

static void read_boot(struct fs *f)
{
    uint8_t bs[512];
    rd(0, bs, sizeof bs);
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0)
        die("%s isn't an exFAT volume.", g_name);
    for (int i = 11; i < 64; i++)
        if (bs[i]) die("%s has a malformed exFAT boot sector.", g_name);
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        die("%s has a malformed exFAT boot sector.", g_name);

    unsigned bps_shift = bs[108], spc_shift = bs[109];
    if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift)
        die("%s has a malformed exFAT boot sector.", g_name);
    if (bs[110] != 1)
        die("%s keeps two FATs (TexFAT); this tool only handles the usual single-FAT exFAT.", g_name);

    f->bps = 1u << bps_shift;
    f->clus = (uint64_t)f->bps << spc_shift;

    /* Boot checksum (exFAT spec 3.4): covers sectors 0-10, skipping VolumeFlags and PercentInUse. */
    size_t n = 12 * (size_t)f->bps;
    uint8_t *region = malloc(n);
    if (!region) die("Out of memory.");
    rd(0, region, n);
    uint32_t sum = 0;
    for (size_t i = 0; i < 11 * (size_t)f->bps; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + region[i];
    }
    for (size_t i = 11 * (size_t)f->bps; i < n; i += 4)
        if (le32(region + i) != sum) die("%s's exFAT boot checksum doesn't match. Nothing was trimmed.", g_name);
    free(region);

    uint64_t vol_bytes = le64(bs + 72) * f->bps;
    uint32_t fat_sector = le32(bs + 80), fat_sectors = le32(bs + 84), heap_sector = le32(bs + 88);
    f->fat_off = (uint64_t)fat_sector * f->bps;
    f->fat_len = (uint64_t)fat_sectors * f->bps;
    f->heap_off = (uint64_t)heap_sector * f->bps;
    f->nclus = le32(bs + 92);
    f->root = le32(bs + 96);
    f->flags = le16(bs + 106);

    if (bs[105] != 1)
        die("%s uses exFAT revision %u.%u; this tool only handles 1.x. Nothing was trimmed.", g_name, bs[105], bs[104]);
    if (f->flags & 4)
        die("%s reports a media failure. Run First Aid in Disk Utility, then try again. Nothing was trimmed.", g_name);
    if (fat_sector < 24 || (uint64_t)heap_sector < (uint64_t)fat_sector + fat_sectors)
        die("%s's exFAT geometry doesn't fit the partition. Nothing was trimmed.", g_name);
    if (f->nclus == 0 || f->nclus > 0xFFFFFFF5u ||
        f->fat_len < ((uint64_t)f->nclus + 2) * 4 ||
        f->heap_off + (uint64_t)f->nclus * f->clus > vol_bytes ||
        vol_bytes > g_devsize ||
        !valid_clus(f, f->root))
        die("%s's exFAT geometry doesn't fit the partition. Nothing was trimmed.", g_name);
}

struct meta {
    uint32_t bitmap_first;
    uint64_t bitmap_len;
    uint32_t upcase_first;
    uint32_t *root_chain;
    uint32_t root_n;
    char label[64];
};

static void utf16_label(const uint8_t *p, unsigned chars, char *out, size_t cap)
{
    size_t o = 0;
    for (unsigned i = 0; i < chars && i < 11; i++) {
        unsigned c = le16(p + 2 * i);
        if (c < 0x80 && o + 1 < cap) out[o++] = (char)c;
        else if (c < 0x800 && o + 2 < cap) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (o + 3 < cap) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

static void read_root(const struct fs *f, struct meta *m)
{
    memset(m, 0, sizeof *m);
    m->root_chain = chain(f, f->root, 0, &m->root_n, "root directory");
    uint8_t *buf = malloc(f->clus);
    if (!buf) die("Out of memory.");
    int bitmaps = 0, done = 0;
    for (uint32_t k = 0; k < m->root_n && !done; k++) {
        rd(clus_off(f, m->root_chain[k]), buf, f->clus);
        for (uint64_t i = 0; i < f->clus; i += 32) {
            const uint8_t *e = buf + i;
            if (e[0] == 0x00) { done = 1; break; }
            if (e[0] == 0x81) {
                if (e[1] & 1) continue;
                bitmaps++;
                m->bitmap_first = le32(e + 20);
                m->bitmap_len = le64(e + 24);
            } else if (e[0] == 0x82) {
                m->upcase_first = le32(e + 20);
            } else if (e[0] == 0x83) {
                utf16_label(e + 2, e[1], m->label, sizeof m->label);
            }
        }
    }
    free(buf);
    if (bitmaps != 1)
        die("Safety check failed: found %d allocation bitmaps, expected 1. Nothing was trimmed.", bitmaps);
    if (m->bitmap_len != ((uint64_t)f->nclus + 7) / 8)
        die("Safety check failed: the allocation bitmap's length doesn't match the volume. Nothing was trimmed.");
}

static uint8_t *read_bitmap(const struct fs *f, const struct meta *m, uint32_t **bm_chain, uint32_t *bm_n)
{
    uint64_t need = ((uint64_t)f->nclus + 7) / 8;
    *bm_chain = chain(f, m->bitmap_first, (m->bitmap_len + f->clus - 1) / f->clus, bm_n, "allocation bitmap");
    uint8_t *bm = malloc(need);
    if (!bm) die("Out of memory.");
    uint64_t got = 0;
    for (uint32_t k = 0; k < *bm_n && got < need;) {
        uint32_t run = 1;
        while (k + run < *bm_n && (*bm_chain)[k + run] == (*bm_chain)[k] + run) run++;
        uint64_t len = (uint64_t)run * f->clus;
        if (len > need - got) len = need - got;
        rd(clus_off(f, (*bm_chain)[k]), bm + got, (size_t)len);
        got += len;
        k += run;
    }
    if (got != need)
        die("Safety check failed: the allocation bitmap's cluster chain is shorter than the bitmap. Nothing was trimmed.");
    return bm;
}

static int allocated(const uint8_t *bm, uint32_t c) { return bm[(c - 2) >> 3] >> ((c - 2) & 7) & 1; }
static int index_free(const uint8_t *bm, uint32_t i) { return !(bm[i >> 3] >> (i & 7) & 1); }

struct vol {
    struct fs f;
    struct meta m;
    uint8_t *bm;
    uint32_t *bm_chain;
    uint32_t bm_n;
    uint64_t free_bytes;
    uint64_t total_bytes;
};

static void load_volume(struct vol *v, int dry, int had_df, uint64_t df_free)
{
    read_boot(&v->f);
    if (!dry && (v->f.flags & 2))
        die("%s wasn't closed cleanly. Run First Aid in Disk Utility, then try again. Nothing was trimmed.", g_name);

    read_root(&v->f, &v->m);
    v->bm = read_bitmap(&v->f, &v->m, &v->bm_chain, &v->bm_n);

    for (uint32_t k = 0; k < v->m.root_n; k++)
        if (!allocated(v->bm, v->m.root_chain[k]))
            die("Safety check failed: the root directory sits in space marked free. Nothing was trimmed.");
    for (uint32_t k = 0; k < v->bm_n; k++)
        if (!allocated(v->bm, v->bm_chain[k]))
            die("Safety check failed: the allocation bitmap sits in space marked free. Nothing was trimmed.");
    if (valid_clus(&v->f, v->m.upcase_first) && !allocated(v->bm, v->m.upcase_first))
        die("Safety check failed: the up-case table sits in space marked free. Nothing was trimmed.");

    uint64_t free_clus = 0;
    for (uint32_t i = 0; i < v->f.nclus; i++)
        if (index_free(v->bm, i)) free_clus++;
    v->total_bytes = (uint64_t)v->f.nclus * v->f.clus;
    v->free_bytes = free_clus * v->f.clus;

    if (had_df) {
        char h1[32], h2[32];
        uint64_t diff = v->free_bytes > df_free ? v->free_bytes - df_free : df_free - v->free_bytes;
        if (diff > v->total_bytes / 100)
            die("Safety check failed: the bitmap shows %s free but macOS reported %s. Nothing was trimmed.",
                human(v->free_bytes, h1, sizeof h1), human(df_free, h2, sizeof h2));
    }
}

struct run {
    uint64_t s, e;
    uint64_t before;
    uint32_t first, n;
};

struct plan {
    struct run *v;
    size_t n, cap;
    uint64_t bytes;
    uint64_t population;
};

static uint64_t align_up(uint64_t x) { return (x + g_align - 1) / g_align * g_align; }
static uint64_t align_down(uint64_t x) { return x / g_align * g_align; }

static void plan_add(struct plan *p, const struct fs *f, uint64_t s, uint64_t e)
{
    uint64_t heap_end = f->heap_off + (uint64_t)f->nclus * f->clus;
    if (s < f->heap_off || e > heap_end) die("Internal error: extent outside the data area. Stopped.");
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 1024;
        if (!(p->v = realloc(p->v, p->cap * sizeof *p->v))) die("Out of memory.");
    }
    uint64_t c0 = (s - f->heap_off + f->clus - 1) / f->clus;
    uint64_t c1 = (e - f->heap_off) / f->clus;
    struct run *r = &p->v[p->n++];
    r->s = s;
    r->e = e;
    r->first = (uint32_t)c0;
    r->n = c1 > c0 ? (uint32_t)(c1 - c0) : 0;
    r->before = p->population;
    p->population += r->n;
    p->bytes += e - s;
}

static void plan_runs(const struct vol *v, struct plan *p)
{
    memset(p, 0, sizeof *p);
    const struct fs *f = &v->f;
    for (uint32_t i = 0; i < f->nclus;) {
        if (v->bm[i >> 3] == 0xFF && (i & 7) == 0) { i += 8; continue; }
        if (!index_free(v->bm, i)) { i++; continue; }
        uint32_t j = i;
        while (j < f->nclus && index_free(v->bm, j)) j++;
        uint64_t s = align_up(clus_off(f, i + 2)), e = align_down(clus_off(f, j + 2));
        if (s < e) plan_add(p, f, s, e);
        i = j;
    }
}

struct sample {
    uint32_t dirty[SAMPLE_TARGET];
    uint32_t ndirty;
    uint32_t draws;
};

static int claim(uint32_t *seen, uint32_t rank)
{
    uint32_t h = (rank * 2654435761u) >> 19;
    while (seen[h]) {
        if (seen[h] == rank + 1) return 0;
        h = (h + 1) & (SEEN_SLOTS - 1);
    }
    seen[h] = rank + 1;
    return 1;
}

static uint32_t locate(const struct plan *p, uint32_t rank)
{
    size_t lo = 0, hi = p->n;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->v[mid].before <= rank) lo = mid;
        else hi = mid;
    }
    return p->v[lo].first + (uint32_t)(rank - p->v[lo].before);
}

static int cluster_is_zero(const struct fs *f, uint32_t index, uint8_t *buf)
{
    size_t len = f->clus < SAMPLE_READ_BYTES ? (size_t)f->clus : SAMPLE_READ_BYTES;
    rd(clus_off(f, index + 2), buf, len);
    for (size_t i = 0; i < len; i++)
        if (buf[i]) return 0;
    return 1;
}

static void draw_sample(const struct fs *f, const struct plan *p, struct sample *s)
{
    memset(s, 0, sizeof *s);
    if (p->population == 0) return;
    uint64_t limit = p->population < SAMPLE_MAX_DRAWS ? p->population : SAMPLE_MAX_DRAWS;
    uint32_t *seen = calloc(SEEN_SLOTS, sizeof *seen);
    uint8_t *buf = malloc(SAMPLE_READ_BYTES);
    if (!seen || !buf) die("Out of memory.");
    while (s->ndirty < SAMPLE_TARGET && s->draws < limit) {
        if (g_stop) die("Stopped.%s", nothing_trimmed());
        uint32_t rank = arc4random_uniform((uint32_t)p->population);
        if (!claim(seen, rank)) continue;
        s->draws++;
        uint32_t index = locate(p, rank);
        if (!cluster_is_zero(f, index, buf)) s->dirty[s->ndirty++] = index;
        if (s->draws % 64 == 0) status("Sampling %s… %u clusters", g_name, s->draws);
    }
    free(seen);
    free(buf);
}

static uint32_t count_cleared(const struct fs *f, const struct sample *s)
{
    uint8_t *buf = malloc(SAMPLE_READ_BYTES);
    if (!buf) die("Out of memory.");
    uint32_t cleared = 0;
    for (uint32_t k = 0; k < s->ndirty; k++)
        if (cluster_is_zero(f, s->dirty[k], buf)) cleared++;
    free(buf);
    return cleared;
}

static void submit(dk_extent_t *ext, int ne, uint64_t bytes)
{
    char h[32];
    dk_unmap_t u;
    memset(&u, 0, sizeof u);
    u.extents = ext;
    u.extentsCount = (uint32_t)ne;
    if (ioctl(g_fd, DKIOCUNMAP, &u) < 0) {
        if (errno == ENOENT)
            die("%s was mounted while exfatrim ran, so macOS refused the TRIM. Your files are untouched.", g_name);
        die("The drive rejected TRIM (%s) after %s. Your files are untouched.", strerror(errno), human(g_trimmed, h, sizeof h));
    }
    g_trimmed += bytes;
}

static void trim_runs(const struct plan *p)
{
    dk_extent_t ext[EXTENTS_PER_CALL];
    int ne = 0;
    uint64_t batch = 0;
    for (size_t k = 0; k < p->n && !g_stop; k++) {
        for (uint64_t s = p->v[k].s; s < p->v[k].e && !g_stop;) {
            uint64_t len = p->v[k].e - s > MAX_EXTENT_BYTES ? MAX_EXTENT_BYTES : p->v[k].e - s;
            ext[ne].offset = s;
            ext[ne].length = len;
            ne++;
            batch += len;
            s += len;
            if (ne == EXTENTS_PER_CALL) {
                submit(ext, ne, batch);
                ne = 0;
                batch = 0;
                status("Trimming %s… %d%%", g_name, p->bytes ? (int)(g_trimmed * 100 / p->bytes) : 100);
            }
        }
    }
    if (ne && !g_stop) submit(ext, ne, batch);
}

static int is_bsd_id(const char *s)
{
    if (strncmp(s, "disk", 4)) return 0;
    const char *p = s + 4;
    if (!(*p >= '0' && *p <= '9')) return 0;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == 's') {
        p++;
        if (!(*p >= '0' && *p <= '9')) return 0;
        while (*p >= '0' && *p <= '9') p++;
    }
    return *p == 0;
}

static const char *strip_from(const char *from)
{
    return strncmp(from, "/dev/", 5) ? from : from + 5;
}

static void adopt_mount(const struct statfs *m, int *mounted, uint64_t *df_free)
{
    if (strcmp(m->f_fstypename, "exfat"))
        die("%s is %s, not exFAT.", m->f_mntonname, m->f_fstypename);
    snprintf(g_bsd, sizeof g_bsd, "%s", strip_from(m->f_mntfromname));
    snprintf(g_name, sizeof g_name, "%s", strrchr(m->f_mntonname, '/') + 1);
    struct statfs st;
    sync();
    if (statfs(m->f_mntonname, &st) < 0)
        die("Can't read free space on %s: %s", g_name, strerror(errno));
    *df_free = (uint64_t)st.f_bavail * st.f_bsize;
    *mounted = 1;
}

static void resolve(const char *arg, int *mounted, uint64_t *df_free)
{
    struct statfs *mnt;
    int n = getmntinfo(&mnt, MNT_NOWAIT);
    *mounted = 0;

    if (!arg) {
        int hits = 0, pick = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(mnt[i].f_fstypename, "exfat")) { hits++; pick = i; }
        if (hits == 0) die("No exFAT volume is mounted. Plug in the drive, or name it: sudo %s \"My SSD\"", g_prog);
        if (hits > 1) {
            fprintf(stderr, "✗ More than one exFAT volume is mounted; name the one to trim:\n");
            for (int i = 0; i < n; i++)
                if (!strcmp(mnt[i].f_fstypename, "exfat"))
                    fprintf(stderr, "    sudo %s \"%s\"\n", g_prog, strrchr(mnt[i].f_mntonname, '/') + 1);
            exit(EXIT_REFUSED);
        }
        adopt_mount(&mnt[pick], mounted, df_free);
        return;
    }

    char want_path[1100];
    if (arg[0] == '/') snprintf(want_path, sizeof want_path, "%s", arg);
    else snprintf(want_path, sizeof want_path, "/Volumes/%s", arg);
    for (int i = 0; i < n; i++)
        if (!strcmp(mnt[i].f_mntonname, want_path)) {
            adopt_mount(&mnt[i], mounted, df_free);
            return;
        }

    const char *id = arg;
    if (!strncmp(id, "/dev/", 5)) id += 5;
    if (!strncmp(id, "rdisk", 5)) id += 1;
    if (!is_bsd_id(id)) die("Can't find a mounted volume called \"%s\".", arg);
    for (int i = 0; i < n; i++)
        if (!strcmp(strip_from(mnt[i].f_mntfromname), id)) {
            adopt_mount(&mnt[i], mounted, df_free);
            return;
        }
    snprintf(g_bsd, sizeof g_bsd, "%s", id);
    snprintf(g_name, sizeof g_name, "%s", id);
}

static int still_mounted(void)
{
    struct statfs *mnt;
    int n = getmntinfo(&mnt, MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        if (!strcmp(strip_from(mnt[i].f_mntfromname), g_bsd)) return 1;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: sudo %s [-n] [-v] [volume]\n"
        "  Unmounts an exFAT volume, TRIMs its free space, and mounts it again.\n"
        "  volume  name (\"My SSD\"), mount point, or disk id (disk4s2).\n"
        "          Omit it when only one exFAT volume is mounted.\n"
        "  -n      dry run: report what would be trimmed; change nothing.\n"
        "  -v      verify: sample up to 299 free clusters that held data before the\n"
        "          trim and report how many the drive now reads as zeros, with a\n"
        "          95%% confidence bound. With -n, report how much sampled free\n"
        "          space still holds deleted data.\n"
        "exit status: 0 success, 1 refused or error, 2 usage, 3 trim not confirmed\n"
        "  (-v), 4 trimmed but the volume did not mount again.\n",
        g_prog);
    exit(EXIT_USAGE);
}

static void require_access(const char *raw, int dry, const char *arg, int verify)
{
    int probe = open(raw, dry ? O_RDONLY : O_RDWR);
    if (probe < 0) {
        if (errno == EACCES || errno == EPERM)
            die("Needs administrator rights. Run:  sudo %s%s%s%s%s%s", g_prog, dry ? " -n" : "",
                verify ? " -v" : "", arg ? " \"" : "", arg ? arg : "", arg ? "\"" : "");
        if (errno == EBUSY) die("Another process holds %s, so exfatrim can't lock it. Nothing changed.", raw);
        die("Can't open %s: %s", raw, strerror(errno));
    }
    uint32_t features = 0;
    int bad = ioctl(probe, DKIOCGETFEATURES, &features) < 0 || !(features & DK_FEATURE_UNMAP);
    close(probe);
    if (bad) die("macOS reports %s can't accept TRIM over this connection. Nothing changed.", g_name);
}

static void unmount_volume(const char *dev)
{
    status("Unmounting %s…", g_name);
    char *um[] = {"diskutil", "unmount", (char *)dev, NULL};
    int rc = run_quiet("/usr/sbin/diskutil", um);
    int mounted_now = still_mounted();
    if (rc != 0 || mounted_now || g_stop) {
        g_owe_remount = !mounted_now;
        if (g_stop) die("Stopped. Nothing was trimmed.");
        die("Couldn't unmount %s. Close anything using it (Finder windows, apps, Terminal tabs in it) and try again.", g_name);
    }
    g_owe_remount = 1;
}

static void check_filesystem(const char *raw)
{
    const char *skip = getenv("EXFATRIM_TEST_SKIP_FSCK");
    if (skip && !strcmp(skip, "1")) return;
    status("Checking %s for errors…", g_name);
    char *fsck[] = {"fsck_exfat", "-n", (char *)raw, NULL};
    int rc = run_quiet("/sbin/fsck_exfat", fsck);
    if (g_stop) die("Stopped. Nothing was trimmed.");
    if (rc != 0)
        die("%s has file-system errors (fsck_exfat exit %d). Run First Aid in Disk Utility, then try again. Nothing was trimmed.", g_name, rc);
}

static void open_device(const char *raw, int dry)
{
    if (dry) {
        g_fd = open(raw, O_RDONLY);
    } else {
        g_fd = open(raw, O_RDWR | O_EXLOCK | O_NONBLOCK);
        if (g_fd < 0 && (errno == EWOULDBLOCK || errno == EBUSY))
            die("Another process holds %s, so exfatrim can't lock it. Nothing was trimmed.", raw);
        if (g_fd >= 0) fcntl(g_fd, F_SETFL, fcntl(g_fd, F_GETFL) & ~O_NONBLOCK);
    }
    if (g_fd < 0) die("Can't open %s: %s", raw, strerror(errno));

    uint64_t nblk = 0;
    if (ioctl(g_fd, DKIOCGETBLOCKSIZE, &g_blk) < 0 || ioctl(g_fd, DKIOCGETBLOCKCOUNT, &nblk) < 0 || g_blk == 0)
        die("Can't read %s's size: %s", raw, strerror(errno));
    g_devsize = nblk * g_blk;

    g_align = g_blk;
    const char *tb = getenv("EXFATRIM_TEST_BLOCK");
    if (tb) {
        char *end;
        unsigned long v = strtoul(tb, &end, 10);
        if (*end || v < g_blk || (v & (v - 1)))
            die("EXFATRIM_TEST_BLOCK must be a power of two of at least %u.", g_blk);
        g_align = (uint32_t)v;
    }
}

static void format_elapsed(const struct timespec *t0, char *took, size_t n)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long secs = (long)(t1.tv_sec - t0->tv_sec);
    if (secs < 60) snprintf(took, n, "%lds", secs);
    else snprintf(took, n, "%ldm %02lds", secs / 60, secs % 60);
}

static void format_next_month(char *mon, size_t n, int *day)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_mon += 1;
    mktime(&tm);
    strftime(mon, n, "%b", &tm);
    *day = tm.tm_mday;
}

static double confidence_bound(uint32_t n)
{
    return pow(CONFIDENCE_TAIL, 1.0 / (double)n);
}

static void print_dry_sample(const struct sample *s)
{
    if (s->draws == 0) {
        printf("  Deleted data: no free clusters to sample.\n");
        return;
    }
    printf("  Deleted data: %u of %u sampled free clusters (%.0f%%) still hold data.\n",
           s->ndirty, s->draws, 100.0 * s->ndirty / s->draws);
}

static void print_verified(const struct sample *s, uint32_t cleared)
{
    if (s->ndirty == 0) {
        if (s->draws == 0)
            printf("  Verified: 0 of 0 dirty free clusters; no free clusters to sample.\n");
        else
            printf("  Verified: 0 of 0 dirty free clusters; all %u sampled free clusters already read as zeros, so an earlier trim worked.\n",
                   s->draws);
        return;
    }
    printf("  Verified: %u of %u dirty free clusters read as zeros; the drive cleared at least %.1f%% of dirty free space (95%% confidence).\n",
           cleared, s->ndirty, floor(confidence_bound(s->ndirty) * 1000.0) / 10.0);
}

int main(int argc, char **argv)
{
    g_prog = argv[0];
    int dry = 0, verify = 0, opt;
    while ((opt = getopt(argc, argv, "nvh")) != -1) {
        if (opt == 'n') dry = 1;
        else if (opt == 'v') verify = 1;
        else usage();
    }
    if (argc - optind > 1) usage();
    const char *arg = optind < argc ? argv[optind] : NULL;

    g_tty = isatty(2);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int mounted;
    uint64_t df_free = 0;
    resolve(arg, &mounted, &df_free);
    int had_df = mounted;

    char raw[80], dev[80];
    snprintf(raw, sizeof raw, "/dev/r%s", g_bsd);
    snprintf(dev, sizeof dev, "/dev/%s", g_bsd);

    require_access(raw, dry, arg, verify);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    if (!dry) {
        if (mounted) unmount_volume(dev);
        check_filesystem(raw);
    }

    open_device(raw, dry);
    status("Reading %s…", g_name);
    struct vol v;
    load_volume(&v, dry, had_df, df_free);
    if (!mounted && v.m.label[0]) snprintf(g_name, sizeof g_name, "%s", v.m.label);

    struct plan plan;
    plan_runs(&v, &plan);

    struct sample sample;
    memset(&sample, 0, sizeof sample);
    if (verify) draw_sample(&v.f, &plan, &sample);

    if (!dry) trim_runs(&plan);

    uint32_t cleared = 0;
    if (!dry && verify && !g_stop) {
        status("Verifying %s…", g_name);
        cleared = count_cleared(&v.f, &sample);
    }
    close(g_fd);
    g_fd = -1;

    char h1[32], h2[32], h3[32];
    if (g_stop && dry) die("Stopped. Nothing changed.");
    if (g_stop) die("Stopped part-way; %s was trimmed. Your files are untouched.", human(g_trimmed, h1, sizeof h1));

    if (!dry) {
        status("Mounting %s…", g_name);
        remount();
        if (g_owe_remount) {
            clear_status();
            fprintf(stderr, "✗ Trimmed %s, but %s did not mount again.\n", human(g_trimmed, h1, sizeof h1), g_name);
            fprintf(stderr, "  Run  diskutil mount %s  or replug the drive.\n", g_bsd);
            return EXIT_NOT_REMOUNTED;
        }
    }
    clear_status();

    char took[32], mon[8];
    int day;
    format_elapsed(&t0, took, sizeof took);
    format_next_month(mon, sizeof mon, &day);

    human(v.free_bytes, h1, sizeof h1);
    human(v.total_bytes, h2, sizeof h2);
    human(dry ? plan.bytes : g_trimmed, h3, sizeof h3);
    if (dry) {
        printf("%s (dry run): would trim %s in %llu range(s). Nothing changed.\n", g_name, h3, (unsigned long long)plan.n);
        printf("  Free to use: %s of %s.\n", h1, h2);
        if (verify) print_dry_sample(&sample);
        return 0;
    }
    printf("✓ %s trimmed: %s of free space handed back to the SSD (%s).\n", g_name, h3, took);
    printf("  Free to use: %s of %s. %s Next trim: ~%s %d.\n", h1, h2,
           mounted ? "Ready to use." : "Left unmounted, as it was.", mon, day);
    if (!verify) return 0;
    if (cleared != sample.ndirty) {
        fflush(stdout);
        fprintf(stderr, "✗ Trim not confirmed: %u of %u dirty free clusters read as zeros afterwards. "
                        "Either the drive ignored TRIM or it does not return zeros for trimmed blocks.\n",
                cleared, sample.ndirty);
        return EXIT_UNCONFIRMED;
    }
    print_verified(&sample, cleared);
    return 0;
}
