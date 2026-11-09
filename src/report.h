#ifndef EXFATRIM_REPORT_H
#define EXFATRIM_REPORT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

void report_init(int tty);

void report_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void report_clear_status(void);
void report_errorv(const char *fmt, va_list ap);

const char *report_human(uint64_t bytes, char *buf, size_t cap);

void report_usage(const char *prog);
void report_choose_volume(const char *prog, const char *const *names, int n);
void report_still_unmounted(const char *name, const char *bsd);
void report_not_remounted(const char *name, const char *bsd, uint64_t trimmed);

void report_dry_run(const char *name, uint64_t would_trim, uint64_t ranges, uint64_t free_bytes, uint64_t total_bytes);
void report_trimmed(const char *name, uint64_t trimmed, const struct timespec *started, uint64_t free_bytes,
                    uint64_t total_bytes, int mounted);
void report_dry_sample(uint32_t ndirty, uint32_t draws);
void report_verified(uint32_t draws, uint32_t ndirty, uint32_t cleared, double bound);
void report_not_confirmed(uint32_t cleared, uint32_t ndirty);

#endif
