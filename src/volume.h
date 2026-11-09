#ifndef EXFATRIM_VOLUME_H
#define EXFATRIM_VOLUME_H

#include <signal.h>
#include <stdint.h>

#include "device.h"

struct volume {
    const char *prog;
    char name[256];
    char bsd[64];
    char where[1100];
    int mounted;
    uint64_t df_free;
    int owe_remount;
    struct device *device;
};

extern struct volume g_volume;
extern volatile sig_atomic_t g_stop;

/*
 * Closes the held device, mounts the volume again when exfatrim unmounted it,
 * prints one error line and exits: EXIT_NOT_REMOUNTED when the volume stays
 * unmounted, else EXIT_REFUSED.
 */
void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

void volume_resolve(const char *arg);
void volume_require_access(const char *raw, int dry, const char *arg, int verify);
void volume_install_signals(void);
void volume_unmount(const char *dev);
void volume_check_filesystem(const char *raw);
void volume_remount(void);

#endif
