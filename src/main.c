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
 * (-v), 4 the volume was unmounted and did not mount again.
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

#include <getopt.h>
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

static const char* nothing_trimmed(const struct trim_result* t) { return t->trimmed ? "" : " Nothing was trimmed."; }

static void die_read(const struct device* d, const struct trim_result* t) __attribute__((noreturn));

static void die_read(const struct device* d, const struct trim_result* t) {
  if (d->nomem)
    die("Out of memory.");
  die("%s%s", d->err, nothing_trimmed(t));
}

static void on_trim_progress(void* ctx, uint64_t trimmed, uint64_t total) {
  (void)ctx;
  report_status("Trimming %s… %d%%", g_volume.name, total ? (int)(trimmed * 100 / total) : 100);
}

static void on_sample_progress(void* ctx, uint32_t draws) {
  (void)ctx;
  report_status("Sampling %s… %u clusters", g_volume.name, draws);
}

struct options {
  int dry;
  int verify;
  int yes;
  double confidence;
  const char* arg;
};

static int parse_options(int argc, char** argv, struct options* o) {
  static const struct option longopts[] = {{"verify", required_argument, NULL, 'V'}, {NULL, 0, NULL, 0}};
  int opt;
  *o = (struct options){.confidence = VERIFY_CONFIDENCE_DEFAULT};
  while ((opt = getopt_long(argc, argv, "nvyh", longopts, NULL)) != -1) {
    if (opt == 'n') {
      o->dry = 1;
    } else if (opt == 'v') {
      o->verify = 1;
    } else if (opt == 'V') {
      o->verify = 1;
      if (verify_parse_confidence(optarg, &o->confidence) != 0)
        return -1;
    } else if (opt == 'y') {
      o->yes = 1;
    } else {
      return -1;
    }
  }
  if (argc - optind > 1)
    return -1;
  o->arg = optind < argc ? argv[optind] : NULL;
  return 0;
}

static void die_verify(enum verify_status vs, const struct device* d, const struct trim_result* t) {
  if (vs == VERIFY_E_NOMEM)
    die("Out of memory.");
  if (vs == VERIFY_E_STOPPED)
    die("Stopped.%s", nothing_trimmed(t));
  if (vs == VERIFY_E_READ)
    die_read(d, t);
}

static void confirm_first_trim(const struct options* o) {
  if (o->dry || o->yes)
    return;
  enum report_confirm answer = report_confirm_first_trim(g_volume.where);
  if (answer == REPORT_CONFIRM_NO)
    die("Cancelled. Nothing changed.");
  if (answer == REPORT_CONFIRM_NO_TTY)
    die("The first trim asks for confirmation. Run exfatrim in a terminal, or pass -y.");
}

static void load_volume(struct device* device, struct exfat_volume* v, int dry, const struct trim_result* trim) {
  struct exfat_source src = {device_read, device, device->size, g_volume.name};
  char reason[256];
  enum exfat_status st = exfat_load(&src, dry, v, reason, sizeof reason);
  if (st == EXFAT_E_READ)
    die_read(device, trim);
  if (st != EXFAT_OK)
    die("%s", reason);

  if (g_volume.mounted && !exfat_free_matches(v, g_volume.df_free)) {
    char h1[32], h2[32];
    die("Safety check failed: the bitmap shows %s free but macOS reported %s. Nothing was trimmed.",
        report_human(v->free_bytes, h1, sizeof h1), report_human(g_volume.df_free, h2, sizeof h2));
  }
}

static void sample_leftovers(struct device* device, const struct exfat_volume* v, const struct walk_plan* plan,
                             uint32_t target, struct verify_sample* sample, const struct trim_result* trim) {
  die_verify(verify_draw(device, &v->fs, plan, target, &g_stop, on_sample_progress, NULL, sample), device, trim);
}

static void trim_free_space(struct device* device, const struct walk_plan* plan, struct trim_result* trim) {
  enum trim_status ts = trim_runs(device, plan, &g_stop, on_trim_progress, NULL, trim);
  char h[32];
  if (ts == TRIM_E_MOUNTED)
    die("%s was mounted while exfatrim ran, so macOS refused the TRIM. Your files are untouched.", g_volume.name);
  if (ts == TRIM_E_REJECTED)
    die("The drive rejected TRIM (%s) after %s. Your files are untouched.", strerror(trim->err_no),
        report_human(trim->trimmed, h, sizeof h));
}

static uint32_t count_cleared(struct device* device, const struct exfat_volume* v, const struct verify_sample* sample,
                              const struct trim_result* trim) {
  uint32_t cleared = 0;
  report_status("Verifying %s…", g_volume.name);
  die_verify(verify_count_cleared(device, &v->fs, sample, &cleared), device, trim);
  return cleared;
}

static void die_if_stopped(int dry, const struct trim_result* trim) {
  char h[32];
  if (g_stop && dry)
    die("Stopped. Nothing changed.");
  if (g_stop)
    die("Stopped part-way; %s was trimmed. Your files are untouched.", report_human(trim->trimmed, h, sizeof h));
}

static int remount(const struct trim_result* trim) {
  report_status("Mounting %s…", g_volume.name);
  volume_remount();
  if (g_volume.owe_remount) {
    report_clear_status();
    report_not_remounted(g_volume.name, g_volume.bsd, trim->trimmed);
    return EXIT_NOT_REMOUNTED;
  }
  return 0;
}

int main(int argc, char** argv) {
  g_volume.prog = argv[0];
  struct options o;
  if (parse_options(argc, argv, &o) != 0) {
    report_usage(g_volume.prog);
    return EXIT_USAGE;
  }

  report_init(isatty(2));
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  volume_resolve(o.arg);

  char raw[80], dev[80];
  snprintf(raw, sizeof raw, "/dev/r%s", g_volume.bsd);
  snprintf(dev, sizeof dev, "/dev/%s", g_volume.bsd);

  volume_require_access(raw, o.dry, o.arg, o.verify);
  confirm_first_trim(&o);
  volume_read_free_space();
  volume_install_signals();

  if (!o.dry) {
    if (g_volume.mounted)
      volume_unmount(dev);
    volume_check_filesystem(raw);
  }

  struct device device = {.fd = -1};
  g_volume.device = &device;
  char err[256];
  if (device_open(&device, raw, o.dry, g_volume.name, err, sizeof err) != 0)
    die("%s", err);
  report_status("Reading %s…", g_volume.name);

  struct trim_result trim = {0};
  struct exfat_volume v;
  load_volume(&device, &v, o.dry, &trim);
  if (!g_volume.mounted && v.meta.label[0])
    snprintf(g_volume.name, sizeof g_volume.name, "%s", v.meta.label);

  struct walk_plan plan;
  char reason[256];
  if (walk_runs(&v.fs, v.bitmap, device.align, &plan, reason, sizeof reason) != WALK_OK)
    die("%s", reason);

  uint32_t target = verify_sample_size(o.confidence);
  struct verify_sample sample;
  memset(&sample, 0, sizeof sample);
  if (o.verify)
    sample_leftovers(&device, &v, &plan, target, &sample, &trim);
  if (!o.dry)
    trim_free_space(&device, &plan, &trim);
  uint32_t cleared = 0;
  if (!o.dry && o.verify && !g_stop)
    cleared = count_cleared(&device, &v, &sample, &trim);
  device_close(&device);
  die_if_stopped(o.dry, &trim);

  if (!o.dry) {
    int rc = remount(&trim);
    if (rc)
      return rc;
  }
  report_clear_status();

  if (o.dry) {
    report_dry_run(g_volume.name, plan.bytes, plan.n, v.free_bytes, v.total_bytes);
    if (o.verify)
      report_dry_sample(sample.ndirty, sample.draws);
    return 0;
  }
  report_trimmed(g_volume.name, trim.trimmed, &t0, v.free_bytes, v.total_bytes, g_volume.mounted);
  if (!o.verify)
    return 0;
  if (cleared != sample.ndirty) {
    report_not_confirmed();
    return EXIT_UNCONFIRMED;
  }
  report_verified(sample.draws, sample.ndirty, target);
  return 0;
}
