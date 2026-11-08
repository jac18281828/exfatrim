/*
 * exfatrim — TRIM the free space of an exFAT volume on macOS.
 *
 * macOS never sends TRIM to exFAT volumes, so an SSD formatted exFAT never
 * learns which blocks deleted files left behind. This tool:
 *
 *   1. unmounts the volume,
 *   2. checks it with Apple's fsck_exfat (refuses to touch a damaged volume),
 *   3. reads exFAT's allocation bitmap and cross-checks it,
 *   4. sends DKIOCUNMAP for the free clusters only,
 *   5. mounts the volume again.
 *
 * Build:  clang -O2 -Wall -Wextra exfatrim.c -o exfatrim
 * Use:    sudo ./exfatrim                  (the one mounted exFAT volume)
 *         sudo ./exfatrim "My SSD"         (by name, mount point or diskNsM)
 *         sudo ./exfatrim -n "My SSD"      (dry run: report only, change nothing)
 */

#include <errno.h>
#include <fcntl.h>
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

#define MAX_EXTENT_BYTES (256ULL << 20) /* split long free runs into 256 MiB extents */
#define EXTENTS_PER_CALL 64

/* ------------------------------------------------------------------ state */

static char g_name[256];      /* what we call the volume in messages */
static char g_bsd[64];        /* "disk4s2" */
static int g_owe_remount;     /* we unmounted it and must mount it again */
static int g_tty;             /* stderr is a terminal: show transient progress */
static volatile sig_atomic_t g_stop;

static int g_fd = -1;
static uint32_t g_blk;        /* device block size */
static uint64_t g_devsize;    /* device size in bytes */

/* ----------------------------------------------------------------- output */

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
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
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
    if (g_owe_remount)
        fprintf(stderr, "  %s is still unmounted: run  diskutil mount %s  or replug it.\n",
                g_name, g_bsd);
    exit(1);
}

/* Decimal units, as Finder shows them: "2.5 TB", "800 MB", "12 GB". */
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

/* ----------------------------------------------------------- raw disk I/O */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

/* Read any byte range; raw devices only accept block-aligned reads. */
static void rd(uint64_t off, void *dst, size_t len)
{
    if (len == 0) return;
    if (off + len < off || off + len > g_devsize)
        die("Tried to read past the end of %s. Nothing was trimmed.", g_name);
    uint64_t s = off / g_blk * g_blk;
    uint64_t e = (off + len + g_blk - 1) / g_blk * g_blk;
    size_t n = (size_t)(e - s);
    void *buf;
    if (posix_memalign(&buf, 4096, n)) die("Out of memory.");
    for (size_t done = 0; done < n;) {
        size_t chunk = n - done > (1u << 20) ? (1u << 20) : n - done;
        ssize_t r = pread(g_fd, (char *)buf + done, chunk, (off_t)(s + done));
        if (r <= 0) die("Read error on %s: %s. Nothing was trimmed.", g_name, r < 0 ? strerror(errno) : "short read");
        done += (size_t)r;
    }
    memcpy(dst, (char *)buf + (off - s), len);
    free(buf);
}

/* ------------------------------------------------------------------ exFAT */

struct fs {
    uint32_t bps;          /* bytes per sector */
    uint64_t clus;         /* bytes per cluster */
    uint64_t fat_off;      /* byte offset of the FAT */
    uint64_t fat_len;      /* bytes */
    uint64_t heap_off;     /* byte offset of cluster 2 */
    uint32_t nclus;        /* clusters in the heap: valid indices 2 .. nclus+1 */
    uint32_t root;         /* first cluster of the root directory */
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

/* Follow a FAT chain for exactly `want` clusters (or to its end if want == 0). */
static uint32_t *chain(const struct fs *f, uint32_t first, uint32_t want, uint32_t *count, const char *what)
{
    uint32_t cap = want ? want : 16, n = 0;
    uint32_t *v = malloc(cap * sizeof *v);
    if (!v) die("Out of memory.");
    uint32_t c = first;
    for (;;) {
        if (!valid_clus(f, c))
            die("Safety check failed: the %s points at cluster %u, outside the volume. Nothing was trimmed.", what, c);
        if (n == cap) {
            if (cap >= f->nclus) die("Safety check failed: the %s's cluster chain loops. Nothing was trimmed.", what);
            cap *= 2;
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
    *count = n;
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
    f->fat_off = (uint64_t)le32(bs + 80) * f->bps;
    f->fat_len = (uint64_t)le32(bs + 84) * f->bps;
    f->heap_off = (uint64_t)le32(bs + 88) * f->bps;
    f->nclus = le32(bs + 92);
    f->root = le32(bs + 96);
    f->flags = le16(bs + 106);

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
            if (e[0] == 0x00) { done = 1; break; }        /* end of directory */
            if (e[0] == 0x81) {                            /* allocation bitmap */
                if (e[1] & 1) continue;                    /* second bitmap: TexFAT only */
                bitmaps++;
                m->bitmap_first = le32(e + 20);
                m->bitmap_len = le64(e + 24);
            } else if (e[0] == 0x82) {                     /* up-case table */
                m->upcase_first = le32(e + 20);
            } else if (e[0] == 0x83) {                     /* volume label */
                utf16_label(e + 2, e[1], m->label, sizeof m->label);
            }
        }
    }
    free(buf);
    if (bitmaps != 1)
        die("Safety check failed: found %d allocation bitmaps, expected 1. Nothing was trimmed.", bitmaps);
    if (m->bitmap_len < ((uint64_t)f->nclus + 7) / 8)
        die("Safety check failed: the allocation bitmap is too small for the volume. Nothing was trimmed.");
}

static uint8_t *read_bitmap(const struct fs *f, const struct meta *m, uint32_t **bm_chain, uint32_t *bm_n)
{
    uint64_t need = ((uint64_t)f->nclus + 7) / 8;
    uint32_t want = (uint32_t)((m->bitmap_len + f->clus - 1) / f->clus);
    *bm_chain = chain(f, m->bitmap_first, want, bm_n, "allocation bitmap");
    uint8_t *bm = malloc(need);
    if (!bm) die("Out of memory.");
    uint64_t got = 0;
    for (uint32_t k = 0; k < *bm_n && got < need;) {
        uint32_t run = 1;                                  /* coalesce contiguous clusters */
        while (k + run < *bm_n && (*bm_chain)[k + run] == (*bm_chain)[k] + run) run++;
        uint64_t len = (uint64_t)run * f->clus;
        if (len > need - got) len = need - got;
        rd(clus_off(f, (*bm_chain)[k]), bm + got, (size_t)len);
        got += len;
        k += run;
    }
    return bm;
}

static int allocated(const uint8_t *bm, uint32_t c) { return bm[(c - 2) >> 3] >> ((c - 2) & 7) & 1; }

/* -------------------------------------------------------- find the volume */

static void resolve(const char *arg, int *mounted, uint64_t *df_free)
{
    struct statfs *mnt;
    int n = getmntinfo(&mnt, MNT_NOWAIT);
    *mounted = 0;

    if (!arg) {                                       /* auto: the one exFAT volume */
        int hits = 0, pick = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(mnt[i].f_fstypename, "exfat")) { hits++; pick = i; }
        if (hits == 0) die("No exFAT volume is mounted. Plug in the drive, or name it: sudo ./exfatrim \"My SSD\"");
        if (hits > 1) {
            fprintf(stderr, "✗ More than one exFAT volume is mounted; name the one to trim:\n");
            for (int i = 0; i < n; i++)
                if (!strcmp(mnt[i].f_fstypename, "exfat"))
                    fprintf(stderr, "    sudo ./exfatrim \"%s\"\n", strrchr(mnt[i].f_mntonname, '/') + 1);
            exit(1);
        }
        arg = mnt[pick].f_mntonname;
    }

    char want_path[1100] = "", want_bsd[64] = "";
    const char *a = arg;
    if (!strncmp(a, "/dev/", 5)) a += 5;
    if (!strncmp(a, "rdisk", 5)) a += 1;
    if (!strncmp(a, "disk", 4)) snprintf(want_bsd, sizeof want_bsd, "%s", a);
    else if (arg[0] == '/') snprintf(want_path, sizeof want_path, "%s", arg);
    else snprintf(want_path, sizeof want_path, "/Volumes/%s", arg);

    for (int i = 0; i < n; i++) {
        const char *from = mnt[i].f_mntfromname;
        if (!strncmp(from, "/dev/", 5)) from += 5;
        if ((want_path[0] && !strcmp(mnt[i].f_mntonname, want_path)) ||
            (want_bsd[0] && !strcmp(from, want_bsd))) {
            if (strcmp(mnt[i].f_fstypename, "exfat"))
                die("%s is %s, not exFAT.", mnt[i].f_mntonname, mnt[i].f_fstypename);
            snprintf(g_bsd, sizeof g_bsd, "%s", from);
            snprintf(g_name, sizeof g_name, "%s", strrchr(mnt[i].f_mntonname, '/') + 1);
            /* getmntinfo's numbers are cached; flush and ask the file system afresh. */
            struct statfs st;
            sync();
            if (statfs(mnt[i].f_mntonname, &st) < 0)
                die("Can't read free space on %s: %s", g_name, strerror(errno));
            *df_free = (uint64_t)st.f_bavail * st.f_bsize;
            *mounted = 1;
            return;
        }
    }
    if (!want_bsd[0]) die("Can't find a mounted volume called \"%s\".", arg);
    snprintf(g_bsd, sizeof g_bsd, "%s", want_bsd);
    snprintf(g_name, sizeof g_name, "%s", want_bsd);
}

static int still_mounted(void)
{
    struct statfs *mnt;
    int n = getmntinfo(&mnt, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        const char *from = mnt[i].f_mntfromname;
        if (!strncmp(from, "/dev/", 5)) from += 5;
        if (!strcmp(from, g_bsd)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
        "usage: sudo ./exfatrim [-n] [volume]\n"
        "  Unmounts an exFAT volume, TRIMs its free space, and mounts it again.\n"
        "  volume  name (\"My SSD\"), mount point, or disk id (disk4s2).\n"
        "          Omit it when only one exFAT volume is mounted.\n"
        "  -n      dry run: report what would be trimmed; change nothing.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int dry = 0, opt;
    while ((opt = getopt(argc, argv, "nh")) != -1) {
        if (opt == 'n') dry = 1;
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

    /* Fail on permissions before unmounting anything. */
    int probe = open(raw, dry ? O_RDONLY : O_RDWR);
    if (probe < 0) {
        if (errno == EACCES || errno == EPERM)
            die("Needs administrator rights. Run:  sudo %s%s%s%s", argv[0],
                dry ? " -n" : "", arg ? " " : "", arg ? arg : "");
        die("Can't open %s: %s", raw, strerror(errno));
    }
    close(probe);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    if (!dry) {
        if (mounted) {
            status("Unmounting %s…", g_name);
            char *um[] = {"diskutil", "unmount", dev, NULL};
            if (run_quiet("/usr/sbin/diskutil", um) != 0 || still_mounted())
                die("Couldn't unmount %s. Close anything using it (Finder windows, apps, Terminal tabs in it) and try again.", g_name);
            g_owe_remount = 1;
        }
        status("Checking %s for errors…", g_name);
        char *fsck[] = {"fsck_exfat", "-n", raw, NULL};
        int rc = run_quiet("/sbin/fsck_exfat", fsck);
        if (rc != 0)
            die("%s has file-system errors (fsck_exfat exit %d). Run First Aid in Disk Utility, then try again. Nothing was trimmed.", g_name, rc);
        if (g_stop) die("Stopped. Nothing was trimmed.");
    }

    g_fd = open(raw, dry ? O_RDONLY : O_RDWR);
    if (g_fd < 0) die("Can't open %s: %s", raw, strerror(errno));
    uint64_t nblk = 0;
    uint32_t features = 0;
    if (ioctl(g_fd, DKIOCGETBLOCKSIZE, &g_blk) < 0 || ioctl(g_fd, DKIOCGETBLOCKCOUNT, &nblk) < 0 || g_blk == 0)
        die("Can't read %s's size: %s", raw, strerror(errno));
    g_devsize = nblk * g_blk;
    if (ioctl(g_fd, DKIOCGETFEATURES, &features) < 0 || !(features & DK_FEATURE_UNMAP))
        die("macOS reports %s can't accept TRIM over this connection. Nothing changed.", g_name);

    status("Reading %s…", g_name);
    struct fs f;
    read_boot(&f);
    if (!dry && (f.flags & 2))
        die("%s wasn't closed cleanly. Run First Aid in Disk Utility, then try again. Nothing was trimmed.", g_name);

    struct meta m;
    read_root(&f, &m);
    if (!mounted && m.label[0]) snprintf(g_name, sizeof g_name, "%s", m.label);
    uint32_t *bm_chain, bm_n;
    uint8_t *bm = read_bitmap(&f, &m, &bm_chain, &bm_n);

    /* Cross-checks: the volume's own metadata must be marked in use. */
    for (uint32_t k = 0; k < m.root_n; k++)
        if (!allocated(bm, m.root_chain[k])) die("Safety check failed: the root directory sits in space marked free. Nothing was trimmed.");
    for (uint32_t k = 0; k < bm_n; k++)
        if (!allocated(bm, bm_chain[k])) die("Safety check failed: the allocation bitmap sits in space marked free. Nothing was trimmed.");
    if (valid_clus(&f, m.upcase_first) && !allocated(bm, m.upcase_first))
        die("Safety check failed: the up-case table sits in space marked free. Nothing was trimmed.");

    uint64_t free_clus = 0;
    for (uint32_t i = 0; i < f.nclus; i++)
        if (!(bm[i >> 3] >> (i & 7) & 1)) free_clus++;
    uint64_t total = (uint64_t)f.nclus * f.clus, free_bytes = free_clus * f.clus;
    char h1[32], h2[32], h3[32];

    /* Cross-check: free space from the bitmap must match what macOS reported while mounted. */
    if (had_df) {
        uint64_t diff = free_bytes > df_free ? free_bytes - df_free : df_free - free_bytes;
        if (diff > total / 100)
            die("Safety check failed: the bitmap shows %s free but macOS reported %s. Nothing was trimmed.",
                human(free_bytes, h1, sizeof h1), human(df_free, h2, sizeof h2));
    }

    /* Walk free runs and TRIM them. */
    dk_extent_t ext[EXTENTS_PER_CALL];
    int ne = 0;
    uint64_t trimmed = 0, ranges = 0, heap_end = f.heap_off + total;
    for (uint32_t i = 0; i < f.nclus && !g_stop;) {
        if (bm[i >> 3] == 0xFF && (i & 7) == 0) { i += 8; continue; }
        if (bm[i >> 3] >> (i & 7) & 1) { i++; continue; }
        uint32_t j = i;
        while (j < f.nclus && !(bm[j >> 3] >> (j & 7) & 1)) j++;
        uint64_t s = clus_off(&f, i + 2), e = clus_off(&f, j + 2);
        s = (s + g_blk - 1) / g_blk * g_blk;              /* shrink to device blocks */
        e = e / g_blk * g_blk;
        ranges++;
        while (s < e) {
            uint64_t len = e - s > MAX_EXTENT_BYTES ? MAX_EXTENT_BYTES : e - s;
            if (s < f.heap_off || s + len > heap_end) die("Internal error: extent outside the data area. Stopped.");
            ext[ne].offset = s;
            ext[ne].length = len;
            ne++;
            trimmed += len;
            s += len;
            if (ne == EXTENTS_PER_CALL) {
                if (!dry) {
                    dk_unmap_t u;
                    memset(&u, 0, sizeof u);
                    u.extents = ext;
                    u.extentsCount = (uint32_t)ne;
                    if (ioctl(g_fd, DKIOCUNMAP, &u) < 0)
                        die("The drive rejected TRIM (%s) after %s. Your files are untouched.",
                            strerror(errno), human(trimmed - len, h1, sizeof h1));
                }
                ne = 0;
                status("%s %s… %d%%", dry ? "Scanning" : "Trimming", g_name,
                       free_bytes ? (int)(trimmed * 100 / free_bytes) : 100);
            }
        }
        i = j;
    }
    if (ne && !dry && !g_stop) {
        dk_unmap_t u;
        memset(&u, 0, sizeof u);
        u.extents = ext;
        u.extentsCount = (uint32_t)ne;
        if (ioctl(g_fd, DKIOCUNMAP, &u) < 0)
            die("The drive rejected TRIM (%s). Your files are untouched.", strerror(errno));
    }
    close(g_fd);
    g_fd = -1;

    if (g_stop) die("Stopped part-way; %s was trimmed. Your files are untouched.", human(trimmed, h1, sizeof h1));

    if (!dry) {
        status("Mounting %s…", g_name);
        remount();
    }
    clear_status();

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long secs = (long)(t1.tv_sec - t0.tv_sec);
    char took[32];
    if (secs < 60) snprintf(took, sizeof took, "%lds", secs);
    else snprintf(took, sizeof took, "%ldm %02lds", secs / 60, secs % 60);

    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_mon += 1;
    mktime(&tm);
    char mon[8];
    strftime(mon, sizeof mon, "%b", &tm);

    human(free_bytes, h1, sizeof h1);
    human(total, h2, sizeof h2);
    human(trimmed, h3, sizeof h3);
    if (dry) {
        printf("%s (dry run): would trim %s in %llu range(s). Nothing changed.\n", g_name, h3, (unsigned long long)ranges);
        printf("  Free to use: %s of %s.\n", h1, h2);
    } else {
        printf("✓ %s trimmed: %s of free space handed back to the SSD (%s).\n", g_name, h3, took);
        const char *state = g_owe_remount ? "Couldn't remount; replug the drive."
                          : mounted       ? "Ready to use."
                                          : "Left unmounted, as it was.";
        printf("  Free to use: %s of %s. %s Next trim: ~%s %d.\n", h1, h2, state, mon, tm.tm_mday);
    }
    return 0;
}
