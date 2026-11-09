#ifndef EXFATRIM_TRIM_H
#define EXFATRIM_TRIM_H

#include <signal.h>
#include <stdint.h>

#include "device.h"
#include "walk.h"

enum trim_status { TRIM_OK = 0, TRIM_E_MOUNTED, TRIM_E_REJECTED };

struct trim_result {
  uint64_t trimmed;
  int err_no;
};

typedef void (*trim_progress_fn)(void* ctx, uint64_t trimmed, uint64_t total);

/*
 * Unmaps every extent of plan in batches of DEVICE_MAX_EXTENTS, adding each
 * batch to result->trimmed only after its ioctl succeeds. Stops between
 * batches once *stop is set.
 */
enum trim_status trim_runs(struct device* d, const struct walk_plan* plan, const volatile sig_atomic_t* stop,
                           trim_progress_fn progress, void* progress_ctx, struct trim_result* result);

#endif
