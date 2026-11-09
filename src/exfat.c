#include "exfat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct loader {
    const struct exfat_source *src;
    char *reason;
    size_t cap;
};

static enum exfat_status refuse(const struct loader *l, enum exfat_status st, const char *fmt, ...)
{
    if (l->reason && l->cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(l->reason, l->cap, fmt, ap);
        va_end(ap);
    }
    return st;
}

static enum exfat_status no_memory(const struct loader *l)
{
    return refuse(l, EXFAT_E_NOMEM, "Out of memory.");
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static enum exfat_status rd(const struct loader *l, uint64_t off, void *dst, size_t len)
{
    if (len == 0) return EXFAT_OK;
    if (l->src->read(l->src->ctx, off, dst, len) != 0) return refuse(l, EXFAT_E_READ, "Couldn't read %s.", l->src->name);
    return EXFAT_OK;
}

int exfat_valid_cluster(const struct exfat_fs *f, uint32_t c)
{
    return c >= 2 && c - 2 < f->nclus;
}

uint64_t exfat_cluster_offset(const struct exfat_fs *f, uint32_t c)
{
    return f->heap_off + (uint64_t)(c - 2) * f->clus;
}

int exfat_cluster_allocated(const uint8_t *bitmap, uint32_t c)
{
    return bitmap[(c - 2) >> 3] >> ((c - 2) & 7) & 1;
}

int exfat_index_free(const uint8_t *bitmap, uint32_t index)
{
    return !(bitmap[index >> 3] >> (index & 7) & 1);
}

static enum exfat_status fat_next(const struct loader *l, const struct exfat_fs *f, uint32_t c, uint32_t *next)
{
    uint8_t b[4];
    enum exfat_status st = rd(l, f->fat_off + (uint64_t)c * 4, b, 4);
    if (st != EXFAT_OK) return st;
    *next = le32(b);
    return EXFAT_OK;
}

static enum exfat_status chain(const struct loader *l, const struct exfat_fs *f, uint32_t first, uint64_t want,
                               uint32_t **out, uint32_t *count, const char *what)
{
    *out = NULL;
    if (want > f->nclus)
        return refuse(l, EXFAT_E_CHAIN_TOO_LONG,
                      "Safety check failed: the %s claims more clusters than the volume has. Nothing was trimmed.",
                      what);
    uint8_t *seen = calloc(((uint64_t)f->nclus + 7) / 8, 1);
    if (!seen) return no_memory(l);
    uint64_t cap = want ? want : 16, n = 0;
    uint32_t *v = malloc(cap * sizeof *v);
    if (!v) {
        free(seen);
        return no_memory(l);
    }
    enum exfat_status st = EXFAT_OK;
    uint32_t c = first;
    for (;;) {
        if (!exfat_valid_cluster(f, c)) {
            st = refuse(l, EXFAT_E_CHAIN_OUTSIDE,
                        "Safety check failed: the %s points at cluster %u, outside the volume. Nothing was trimmed.",
                        what, c);
            break;
        }
        if (seen[(c - 2) >> 3] >> ((c - 2) & 7) & 1) {
            st = refuse(l, EXFAT_E_CHAIN_LOOP,
                        "Safety check failed: the %s's cluster chain loops. Nothing was trimmed.", what);
            break;
        }
        seen[(c - 2) >> 3] |= (uint8_t)(1u << ((c - 2) & 7));
        if (n == cap) {
            cap *= 2;
            if (cap > f->nclus) cap = f->nclus;
            uint32_t *grown = realloc(v, cap * sizeof *v);
            if (!grown) {
                st = no_memory(l);
                break;
            }
            v = grown;
        }
        v[n++] = c;
        if (want && n == want) break;
        uint32_t next;
        st = fat_next(l, f, c, &next);
        if (st != EXFAT_OK) break;
        if (next == 0xFFFFFFFFu) {
            if (want)
                st = refuse(l, EXFAT_E_CHAIN_EARLY_END,
                            "Safety check failed: the %s's cluster chain ends early. Nothing was trimmed.", what);
            break;
        }
        c = next;
    }
    free(seen);
    if (st != EXFAT_OK) {
        free(v);
        return st;
    }
    *out = v;
    *count = (uint32_t)n;
    return EXFAT_OK;
}

static enum exfat_status read_boot(const struct loader *l, struct exfat_fs *f)
{
    const char *name = l->src->name;
    enum exfat_status st;
    uint8_t bs[512];
    if ((st = rd(l, 0, bs, sizeof bs)) != EXFAT_OK) return st;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0) return refuse(l, EXFAT_E_NOT_EXFAT, "%s isn't an exFAT volume.", name);
    for (int i = 11; i < 64; i++)
        if (bs[i]) return refuse(l, EXFAT_E_BOOT_MALFORMED, "%s has a malformed exFAT boot sector.", name);
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return refuse(l, EXFAT_E_BOOT_MALFORMED, "%s has a malformed exFAT boot sector.", name);

    unsigned bps_shift = bs[108], spc_shift = bs[109];
    if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift)
        return refuse(l, EXFAT_E_BOOT_MALFORMED, "%s has a malformed exFAT boot sector.", name);
    if (bs[110] != 1)
        return refuse(l, EXFAT_E_TEXFAT, "%s keeps two FATs (TexFAT); this tool only handles the usual single-FAT exFAT.",
                      name);

    f->bps = 1u << bps_shift;
    f->clus = (uint64_t)f->bps << spc_shift;

    /* Boot checksum (exFAT spec 3.4): covers sectors 0-10, skipping VolumeFlags and PercentInUse. */
    size_t n = 12 * (size_t)f->bps;
    uint8_t *region = malloc(n);
    if (!region) return no_memory(l);
    if ((st = rd(l, 0, region, n)) != EXFAT_OK) {
        free(region);
        return st;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < 11 * (size_t)f->bps; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + region[i];
    }
    for (size_t i = 11 * (size_t)f->bps; i < n; i += 4) {
        if (le32(region + i) != sum) {
            free(region);
            return refuse(l, EXFAT_E_CHECKSUM, "%s's exFAT boot checksum doesn't match. Nothing was trimmed.", name);
        }
    }
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
        return refuse(l, EXFAT_E_REVISION, "%s uses exFAT revision %u.%u; this tool only handles 1.x. Nothing was trimmed.",
                      name, bs[105], bs[104]);
    if (f->flags & 4)
        return refuse(l, EXFAT_E_MEDIA_FAILURE,
                      "%s reports a media failure. Run First Aid in Disk Utility, then try again. Nothing was trimmed.",
                      name);
    if (fat_sector < 24)
        return refuse(l, EXFAT_E_FAT_OFFSET, "%s's exFAT geometry doesn't fit the partition. Nothing was trimmed.", name);
    if ((uint64_t)heap_sector < (uint64_t)fat_sector + fat_sectors)
        return refuse(l, EXFAT_E_HEAP_OVERLAPS_FAT,
                      "%s's exFAT geometry doesn't fit the partition. Nothing was trimmed.", name);
    if (f->nclus == 0 || f->nclus > 0xFFFFFFF5u || f->fat_len < ((uint64_t)f->nclus + 2) * 4 ||
        f->heap_off + (uint64_t)f->nclus * f->clus > vol_bytes || vol_bytes > l->src->size ||
        !exfat_valid_cluster(f, f->root))
        return refuse(l, EXFAT_E_GEOMETRY, "%s's exFAT geometry doesn't fit the partition. Nothing was trimmed.", name);
    return EXFAT_OK;
}

static void utf16_label(const uint8_t *p, unsigned chars, char *out, size_t cap)
{
    size_t o = 0;
    for (unsigned i = 0; i < chars && i < 11; i++) {
        unsigned c = le16(p + 2 * i);
        if (c < 0x80 && o + 1 < cap) {
            out[o++] = (char)c;
        } else if (c < 0x800 && o + 2 < cap) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (o + 3 < cap) {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

static enum exfat_status read_root(const struct loader *l, const struct exfat_fs *f, struct exfat_meta *m)
{
    enum exfat_status st = chain(l, f, f->root, 0, &m->root_chain, &m->root_n, "root directory");
    if (st != EXFAT_OK) return st;
    uint8_t *buf = malloc(f->clus);
    if (!buf) return no_memory(l);
    int bitmaps = 0, done = 0;
    for (uint32_t k = 0; k < m->root_n && !done; k++) {
        if ((st = rd(l, exfat_cluster_offset(f, m->root_chain[k]), buf, f->clus)) != EXFAT_OK) {
            free(buf);
            return st;
        }
        for (uint64_t i = 0; i < f->clus; i += 32) {
            const uint8_t *e = buf + i;
            if (e[0] == 0x00) {
                done = 1;
                break;
            }
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
        return refuse(l, EXFAT_E_BITMAP_COUNT,
                      "Safety check failed: found %d allocation bitmaps, expected 1. Nothing was trimmed.", bitmaps);
    if (m->bitmap_len != ((uint64_t)f->nclus + 7) / 8)
        return refuse(l, EXFAT_E_BITMAP_LENGTH,
                      "Safety check failed: the allocation bitmap's length doesn't match the volume. Nothing was trimmed.");
    return EXFAT_OK;
}

static enum exfat_status read_bitmap(const struct loader *l, struct exfat_volume *v)
{
    const struct exfat_fs *f = &v->fs;
    uint64_t need = ((uint64_t)f->nclus + 7) / 8;
    enum exfat_status st = chain(l, f, v->meta.bitmap_first, (v->meta.bitmap_len + f->clus - 1) / f->clus,
                                 &v->bitmap_chain, &v->bitmap_n, "allocation bitmap");
    if (st != EXFAT_OK) return st;
    v->bitmap = malloc(need);
    if (!v->bitmap) return no_memory(l);
    uint64_t got = 0;
    for (uint32_t k = 0; k < v->bitmap_n && got < need;) {
        uint32_t run = 1;
        while (k + run < v->bitmap_n && v->bitmap_chain[k + run] == v->bitmap_chain[k] + run) run++;
        uint64_t len = (uint64_t)run * f->clus;
        if (len > need - got) len = need - got;
        if ((st = rd(l, exfat_cluster_offset(f, v->bitmap_chain[k]), v->bitmap + got, (size_t)len)) != EXFAT_OK)
            return st;
        got += len;
        k += run;
    }
    if (got != need)
        return refuse(l, EXFAT_E_BITMAP_SHORT,
                      "Safety check failed: the allocation bitmap's cluster chain is shorter than the bitmap. Nothing was trimmed.");
    return EXFAT_OK;
}

static enum exfat_status load(const struct loader *l, int allow_dirty, struct exfat_volume *v)
{
    enum exfat_status st = read_boot(l, &v->fs);
    if (st != EXFAT_OK) return st;
    if (!allow_dirty && (v->fs.flags & 2))
        return refuse(l, EXFAT_E_DIRTY,
                      "%s wasn't closed cleanly. Run First Aid in Disk Utility, then try again. Nothing was trimmed.",
                      l->src->name);

    if ((st = read_root(l, &v->fs, &v->meta)) != EXFAT_OK) return st;
    if ((st = read_bitmap(l, v)) != EXFAT_OK) return st;

    for (uint32_t k = 0; k < v->meta.root_n; k++)
        if (!exfat_cluster_allocated(v->bitmap, v->meta.root_chain[k]))
            return refuse(l, EXFAT_E_ROOT_FREE,
                          "Safety check failed: the root directory sits in space marked free. Nothing was trimmed.");
    for (uint32_t k = 0; k < v->bitmap_n; k++)
        if (!exfat_cluster_allocated(v->bitmap, v->bitmap_chain[k]))
            return refuse(l, EXFAT_E_BITMAP_FREE,
                          "Safety check failed: the allocation bitmap sits in space marked free. Nothing was trimmed.");
    if (exfat_valid_cluster(&v->fs, v->meta.upcase_first) && !exfat_cluster_allocated(v->bitmap, v->meta.upcase_first))
        return refuse(l, EXFAT_E_UPCASE_FREE,
                      "Safety check failed: the up-case table sits in space marked free. Nothing was trimmed.");

    uint64_t free_clus = 0;
    for (uint32_t i = 0; i < v->fs.nclus; i++)
        if (exfat_index_free(v->bitmap, i)) free_clus++;
    v->total_bytes = (uint64_t)v->fs.nclus * v->fs.clus;
    v->free_bytes = free_clus * v->fs.clus;
    return EXFAT_OK;
}

enum exfat_status exfat_load(const struct exfat_source *src, int allow_dirty, struct exfat_volume *v, char *reason,
                             size_t reason_cap)
{
    struct loader l = {src, reason, reason_cap};
    memset(v, 0, sizeof *v);
    if (reason && reason_cap) reason[0] = 0;
    enum exfat_status st = load(&l, allow_dirty, v);
    if (st != EXFAT_OK) exfat_release(v);
    return st;
}

void exfat_release(struct exfat_volume *v)
{
    free(v->meta.root_chain);
    free(v->bitmap);
    free(v->bitmap_chain);
    memset(v, 0, sizeof *v);
}
