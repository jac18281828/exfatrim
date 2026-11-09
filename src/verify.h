#ifndef EXFATRIM_VERIFY_H
#define EXFATRIM_VERIFY_H

#include <signal.h>
#include <stdint.h>

#include "device.h"
#include "exfat.h"
#include "walk.h"

#define VERIFY_CONFIDENCE_DEFAULT 95.0
#define VERIFY_CONFIDENCE_MIN 50.0
#define VERIFY_CONFIDENCE_MAX 99.9
#define VERIFY_SAMPLE_MAX 688

struct verify_sample {
  uint32_t dirty[VERIFY_SAMPLE_MAX];
  uint32_t ndirty;
  uint32_t draws;
};

enum verify_status { VERIFY_OK = 0, VERIFY_E_NOMEM, VERIFY_E_READ, VERIFY_E_STOPPED };

typedef void (*verify_progress_fn)(void* ctx, uint32_t draws);

enum verify_status verify_draw(struct device* d, const struct exfat_fs* f, const struct walk_plan* plan,
                               uint32_t target, const volatile sig_atomic_t* stop, verify_progress_fn progress,
                               void* progress_ctx, struct verify_sample* s);

enum verify_status verify_count_cleared(struct device* d, const struct exfat_fs* f, const struct verify_sample* s,
                                        uint32_t* cleared);

uint32_t verify_sample_size(double confidence_pct);

int verify_parse_confidence(const char* text, double* confidence_pct);

#endif
