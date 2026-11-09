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

typedef void (*verify_progress_fn)(void* ctx, uint32_t draws);

enum verify_status verify_draw(struct device* d, const struct exfat_fs* f, const struct walk_plan* plan,
                               const volatile sig_atomic_t* stop, verify_progress_fn progress, void* progress_ctx,
                               struct verify_sample* s);

enum verify_status verify_count_cleared(struct device* d, const struct exfat_fs* f, const struct verify_sample* s,
                                        uint32_t* cleared);

double verify_bound(uint32_t n);

#endif
