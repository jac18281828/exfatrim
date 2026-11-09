#ifndef EXFATRIM_WALK_H
#define EXFATRIM_WALK_H

#include <stddef.h>
#include <stdint.h>

#include "exfat.h"

struct walk_run {
  uint64_t s, e;
  uint64_t before;
  uint32_t first, n;
};

struct walk_plan {
  struct walk_run* v;
  size_t n, cap;
  uint64_t bytes;
  uint64_t population;
};

enum walk_status { WALK_OK = 0, WALK_E_NOMEM, WALK_E_OUTSIDE, WALK_E_ALIGN };

/*
 * Walks the free runs of bitmap and shrinks each inward to the alignment unit,
 * so no extent reaches a byte outside a free cluster. Never exits and never
 * prints; a failure leaves plan empty and writes its text to reason.
 */
enum walk_status walk_runs(const struct exfat_fs* f, const uint8_t* bitmap, uint64_t align, struct walk_plan* plan,
                           char* reason, size_t reason_cap);

void walk_release(struct walk_plan* plan);

#endif
