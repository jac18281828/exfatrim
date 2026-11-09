#include "trim.h"

#include <errno.h>
#include <string.h>

#define MAX_EXTENT_BYTES (256ULL << 20)

static enum trim_status submit(struct device *d, const struct device_extent *ext, size_t n, uint64_t bytes,
                               struct trim_result *r)
{
    int rc = device_unmap(d, ext, n);
    if (rc != 0) {
        r->err_no = rc;
        return rc == ENOENT ? TRIM_E_MOUNTED : TRIM_E_REJECTED;
    }
    r->trimmed += bytes;
    return TRIM_OK;
}

enum trim_status trim_runs(struct device *d, const struct walk_plan *p, const volatile sig_atomic_t *stop,
                           trim_progress_fn progress, void *progress_ctx, struct trim_result *r)
{
    struct device_extent ext[DEVICE_MAX_EXTENTS];
    size_t ne = 0;
    uint64_t batch = 0;
    memset(r, 0, sizeof *r);
    for (size_t k = 0; k < p->n && !*stop; k++) {
        for (uint64_t s = p->v[k].s; s < p->v[k].e && !*stop;) {
            uint64_t len = p->v[k].e - s > MAX_EXTENT_BYTES ? MAX_EXTENT_BYTES : p->v[k].e - s;
            ext[ne].offset = s;
            ext[ne].length = len;
            ne++;
            batch += len;
            s += len;
            if (ne == DEVICE_MAX_EXTENTS) {
                enum trim_status st = submit(d, ext, ne, batch, r);
                if (st != TRIM_OK) return st;
                ne = 0;
                batch = 0;
                if (progress) progress(progress_ctx, r->trimmed, p->bytes);
            }
        }
    }
    if (ne && !*stop) return submit(d, ext, ne, batch, r);
    return TRIM_OK;
}
