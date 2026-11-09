#include "verify.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MAX_DRAWS 3000
#define SAMPLE_READ_BYTES (1u << 20)
#define SEEN_SLOTS 8192
#define CONFIDENCE_TAIL 0.05

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

static uint32_t locate(const struct walk_plan *p, uint32_t rank)
{
    size_t lo = 0, hi = p->n;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->v[mid].before <= rank) lo = mid;
        else hi = mid;
    }
    return p->v[lo].first + (uint32_t)(rank - p->v[lo].before);
}

static enum verify_status cluster_is_zero(struct device *d, const struct exfat_fs *f, uint32_t index, uint8_t *buf,
                                          int *zero)
{
    size_t len = f->clus < SAMPLE_READ_BYTES ? (size_t)f->clus : SAMPLE_READ_BYTES;
    if (device_read(d, exfat_cluster_offset(f, index + 2), buf, len) != 0) return VERIFY_E_READ;
    *zero = 1;
    for (size_t i = 0; i < len; i++)
        if (buf[i]) {
            *zero = 0;
            break;
        }
    return VERIFY_OK;
}

enum verify_status verify_draw(struct device *d, const struct exfat_fs *f, const struct walk_plan *p,
                               const volatile sig_atomic_t *stop, verify_progress_fn progress, void *progress_ctx,
                               struct verify_sample *s)
{
    memset(s, 0, sizeof *s);
    if (p->population == 0) return VERIFY_OK;
    uint64_t limit = p->population < SAMPLE_MAX_DRAWS ? p->population : SAMPLE_MAX_DRAWS;
    uint32_t *seen = calloc(SEEN_SLOTS, sizeof *seen);
    uint8_t *buf = malloc(SAMPLE_READ_BYTES);
    enum verify_status st = VERIFY_OK;
    if (!seen || !buf) st = VERIFY_E_NOMEM;
    while (st == VERIFY_OK && s->ndirty < VERIFY_SAMPLE_TARGET && s->draws < limit) {
        if (*stop) {
            st = VERIFY_E_STOPPED;
            break;
        }
        uint32_t rank = arc4random_uniform((uint32_t)p->population);
        if (!claim(seen, rank)) continue;
        s->draws++;
        uint32_t index = locate(p, rank);
        int zero;
        if ((st = cluster_is_zero(d, f, index, buf, &zero)) != VERIFY_OK) break;
        if (!zero) s->dirty[s->ndirty++] = index;
        if (progress && s->draws % 64 == 0) progress(progress_ctx, s->draws);
    }
    free(seen);
    free(buf);
    return st;
}

enum verify_status verify_count_cleared(struct device *d, const struct exfat_fs *f, const struct verify_sample *s,
                                        uint32_t *cleared)
{
    uint8_t *buf = malloc(SAMPLE_READ_BYTES);
    if (!buf) return VERIFY_E_NOMEM;
    enum verify_status st = VERIFY_OK;
    *cleared = 0;
    for (uint32_t k = 0; k < s->ndirty; k++) {
        int zero;
        if ((st = cluster_is_zero(d, f, s->dirty[k], buf, &zero)) != VERIFY_OK) break;
        if (zero) (*cleared)++;
    }
    free(buf);
    return st;
}

double verify_bound(uint32_t n)
{
    return pow(CONFIDENCE_TAIL, 1.0 / (double)n);
}
