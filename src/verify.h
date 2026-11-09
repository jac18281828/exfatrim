#ifndef EXFATRIM_VERIFY_H
#define EXFATRIM_VERIFY_H

#include <signal.h>
#include <stdint.h>

#include "device.h"
#include "exfat.h"
#include "walk.h"

#define VERIFY_SAMPLE_TARGET 299

struct verify_sample {
    uint32_t dirty[VERIFY_SAMPLE_TARGET];
    uint32_t ndirty;
    uint32_t draws;
};

enum verify_status { VERIFY_OK = 0, VERIFY_E_NOMEM, VERIFY_E_READ, VERIFY_E_STOPPED };

typedef void (*verify_progress_fn)(void *ctx, uint32_t draws);

/*
 * Draws distinct free clusters uniformly from the clusters that lie wholly
 * inside an extent of plan, until VERIFY_SAMPLE_TARGET of them hold a nonzero
 * byte or min(3000, population) draws are spent. A read failure leaves its
 * reason in d->err.
 */
enum verify_status verify_draw(struct device *d, const struct exfat_fs *f, const struct walk_plan *plan,
                               const volatile sig_atomic_t *stop, verify_progress_fn progress, void *progress_ctx,
                               struct verify_sample *s);

/* Re-reads the drawn clusters and counts those that now read as zeros. */
enum verify_status verify_count_cleared(struct device *d, const struct exfat_fs *f, const struct verify_sample *s,
                                        uint32_t *cleared);

/* The share of leftovers cleared, at 95% confidence, when all n read as zeros. */
double verify_bound(uint32_t n);

#endif
