/*
 * exfatrim — TRIM the free space of an exFAT volume on macOS.
 *
 * macOS never sends TRIM to exFAT volumes, so an SSD formatted exFAT never
 * learns which blocks deleted files left behind. This tool:
 *
 *   1. unmounts the volume,
 *   2. checks it with Apple's fsck_exfat (refuses to touch a damaged volume),
 *   3. locks the raw device, reads exFAT's allocation bitmap and cross-checks it,
 *   4. with -v, samples free clusters that hold leftovers,
 *   5. sends DKIOCUNMAP for the free clusters only,
 *   6. with -v, re-reads the sample: a trimmed cluster reads as zeros,
 *   7. mounts the volume again.
 *
 * Invariant: no byte outside a cluster the bitmap marks free, read on the
 * unmounted volume, is ever unmapped.
 *
 * Build:  make            (clang, C17 with -pedantic -Werror; sources in src/)
 * Use:    sudo exfatrim                    (the one mounted exFAT volume)
 *         sudo exfatrim SSD                (by name, mount point or diskNsM)
 *         sudo exfatrim -n SSD             (dry run: report only, change nothing)
 *         sudo exfatrim -v SSD             (trim, then verify the drive dropped the leftovers)
 *         sudo exfatrim -y SSD             (trim without the first-run confirmation)
 *
 * Exit status: 0 success, 1 refused or error, 2 usage, 3 trim not confirmed
 * (-v), 4 trimmed but the volume did not mount again.
 *
 * Test hooks (environment): EXFATRIM_TEST_BLOCK raises the alignment unit to
 * a power of two at or above the device block size; EXFATRIM_TEST_SKIP_FSCK=1
 * skips the fsck_exfat gate; EXFATRIM_TEST_LECTURE_DIR replaces
 * /var/db/exfatrim/lectured, the directory of first-run markers.
 *
 * Before the first trim per user, exfatrim shows a notice and waits for y; the
 * answer writes an empty marker file named for the user (SUDO_USER, else the
 * real uid's name). -y skips the notice and writes no marker; -n never asks.
 */

#include "main.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device.h"
#include "exfat.h"
#include "report.h"
#include "trim.h"
#include "verify.h"
#include "volume.h"
#include "walk.h"

static const char *nothing_trimmed(const struct trim_result *t)
{
    return t->trimmed ? "" : " Nothing was trimmed.";
}

static void die_read(const struct device *d, const struct trim_result *t) __attribute__((noreturn));

static void die_read(const struct device *d, const struct trim_result *t)
{
    if (d->nomem) die("Out of memory.");
    die("%s%s", d->err, nothing_trimmed(t));
}

static void on_trim_progress(void *ctx, uint64_t trimmed, uint64_t total)
{
    (void)ctx;
    report_status("Trimming %s… %d%%", g_volume.name, total ? (int)(trimmed * 100 / total) : 100);
}

static void on_sample_progress(void *ctx, uint32_t draws)
{
    (void)ctx;
    report_status("Sampling %s… %u clusters", g_volume.name, draws);
}

static void load_volume(struct device *device, struct exfat_volume *v, int dry, const struct trim_result *trim)
{
    struct exfat_source src = {device_read, device, device->size, g_volume.name};
    char reason[256];
    enum exfat_status st = exfat_load(&src, dry, v, reason, sizeof reason);
    if (st == EXFAT_E_READ) die_read(device, trim);
    if (st != EXFAT_OK) die("%s", reason);

    if (g_volume.mounted) {
        char h1[32], h2[32];
        uint64_t df = g_volume.df_free;
        uint64_t diff = v->free_bytes > df ? v->free_bytes - df : df - v->free_bytes;
        if (diff > v->total_bytes / 100)
            die("Safety check failed: the bitmap shows %s free but macOS reported %s. Nothing was trimmed.",
                report_human(v->free_bytes, h1, sizeof h1), report_human(df, h2, sizeof h2));
    }
}

int main(int argc, char **argv)
{
    g_volume.prog = argv[0];
    int dry = 0, verify = 0, yes = 0, opt;
    while ((opt = getopt(argc, argv, "nvyh")) != -1) {
        if (opt == 'n') {
            dry = 1;
        } else if (opt == 'v') {
            verify = 1;
        } else if (opt == 'y') {
            yes = 1;
        } else {
            report_usage(g_volume.prog);
            return EXIT_USAGE;
        }
    }
    if (argc - optind > 1) {
        report_usage(g_volume.prog);
        return EXIT_USAGE;
    }
    const char *arg = optind < argc ? argv[optind] : NULL;

    report_init(isatty(2));
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    volume_resolve(arg);

    char raw[80], dev[80];
    snprintf(raw, sizeof raw, "/dev/r%s", g_volume.bsd);
    snprintf(dev, sizeof dev, "/dev/%s", g_volume.bsd);

    volume_require_access(raw, dry, arg, verify);

    if (!dry && !yes) {
        enum report_confirm answer = report_confirm_first_trim(g_volume.where);
        if (answer == REPORT_CONFIRM_NO) die("Cancelled. Nothing changed.");
        if (answer == REPORT_CONFIRM_NO_TTY)
            die("The first trim asks for confirmation. Run exfatrim in a terminal, or pass -y.");
    }

    volume_install_signals();

    if (!dry) {
        if (g_volume.mounted) volume_unmount(dev);
        volume_check_filesystem(raw);
    }

    struct device device = {.fd = -1};
    g_volume.device = &device;
    char err[256];
    if (device_open(&device, raw, dry, g_volume.name, err, sizeof err) != 0) die("%s", err);
    report_status("Reading %s…", g_volume.name);

    struct trim_result trim = {0};
    struct exfat_volume v;
    load_volume(&device, &v, dry, &trim);
    if (!g_volume.mounted && v.meta.label[0]) snprintf(g_volume.name, sizeof g_volume.name, "%s", v.meta.label);

    struct walk_plan plan;
    char reason[256];
    if (walk_runs(&v.fs, v.bitmap, device.align, &plan, reason, sizeof reason) != WALK_OK) die("%s", reason);

    struct verify_sample sample;
    memset(&sample, 0, sizeof sample);
    if (verify) {
        enum verify_status vs = verify_draw(&device, &v.fs, &plan, &g_stop, on_sample_progress, NULL, &sample);
        if (vs == VERIFY_E_NOMEM) die("Out of memory.");
        if (vs == VERIFY_E_STOPPED) die("Stopped.%s", nothing_trimmed(&trim));
        if (vs == VERIFY_E_READ) die_read(&device, &trim);
    }

    if (!dry) {
        enum trim_status ts = trim_runs(&device, &plan, &g_stop, on_trim_progress, NULL, &trim);
        char h[32];
        if (ts == TRIM_E_MOUNTED)
            die("%s was mounted while exfatrim ran, so macOS refused the TRIM. Your files are untouched.",
                g_volume.name);
        if (ts == TRIM_E_REJECTED)
            die("The drive rejected TRIM (%s) after %s. Your files are untouched.", strerror(trim.err_no),
                report_human(trim.trimmed, h, sizeof h));
    }

    uint32_t cleared = 0;
    if (!dry && verify && !g_stop) {
        report_status("Verifying %s…", g_volume.name);
        enum verify_status vs = verify_count_cleared(&device, &v.fs, &sample, &cleared);
        if (vs == VERIFY_E_NOMEM) die("Out of memory.");
        if (vs == VERIFY_E_READ) die_read(&device, &trim);
    }
    device_close(&device);

    char h1[32];
    if (g_stop && dry) die("Stopped. Nothing changed.");
    if (g_stop)
        die("Stopped part-way; %s was trimmed. Your files are untouched.", report_human(trim.trimmed, h1, sizeof h1));

    if (!dry) {
        report_status("Mounting %s…", g_volume.name);
        volume_remount();
        if (g_volume.owe_remount) {
            report_clear_status();
            report_not_remounted(g_volume.name, g_volume.bsd, trim.trimmed);
            return EXIT_NOT_REMOUNTED;
        }
    }
    report_clear_status();

    if (dry) {
        report_dry_run(g_volume.name, plan.bytes, plan.n, v.free_bytes, v.total_bytes);
        if (verify) report_dry_sample(sample.ndirty, sample.draws);
        return 0;
    }
    report_trimmed(g_volume.name, trim.trimmed, &t0, v.free_bytes, v.total_bytes, g_volume.mounted);
    if (!verify) return 0;
    if (cleared != sample.ndirty) {
        report_not_confirmed(cleared, sample.ndirty);
        return EXIT_UNCONFIRMED;
    }
    report_verified(sample.draws, sample.ndirty, cleared, sample.ndirty ? verify_bound(sample.ndirty) : 0.0);
    return 0;
}
