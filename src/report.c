#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LECTURE_DIR "/var/db/exfatrim/lectured"

static int g_tty;

void report_init(int tty)
{
    g_tty = tty;
}

void report_status(const char *fmt, ...)
{
    if (!g_tty) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("\r\033[K", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void report_clear_status(void)
{
    if (g_tty) {
        fputs("\r\033[K", stderr);
        fflush(stderr);
    }
}

void report_errorv(const char *fmt, va_list ap)
{
    fputs("✗ ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

const char *report_human(uint64_t b, char *buf, size_t n)
{
    static const char *unit[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    if (b < 1000) {
        snprintf(buf, n, "%llu bytes", (unsigned long long)b);
        return buf;
    }
    double v = (double)b;
    int i = 0;
    while (v >= 1000 && i < 6) {
        v /= 1000;
        i++;
    }
    if (v >= 999.5 && i < 6) {
        v /= 1000;
        i++;
    }
    snprintf(buf, n, v < 9.95 ? "%.1f %s" : "%.0f %s", v, unit[i]);
    return buf;
}

void report_usage(const char *prog)
{
    fprintf(stderr,
            "usage: sudo %s [-n] [-v] [-y] [volume]\n"
            "  Unmounts an exFAT volume, TRIMs its free space, and mounts it again.\n"
            "  volume  name (SSD), mount point (/Volumes/SSD), or disk id (disk4s2).\n"
            "          Omit it when only one exFAT volume is mounted.\n"
            "  -n      dry run: report what would be trimmed; change nothing.\n"
            "  -v      verify: sample up to 299 free clusters that held leftovers before the\n"
            "          trim and report how many the drive now reads as zeros, with a\n"
            "          95%% confidence bound. With -n, report how much sampled free\n"
            "          space still holds leftovers.\n"
            "  -y      skip the first-run confirmation; write no marker.\n"
            "exit status: 0 success, 1 refused or error, 2 usage, 3 trim not confirmed\n"
            "  (-v), 4 the volume was unmounted and did not mount again.\n",
            prog);
}

void report_choose_volume(const char *prog, const char *const *names, int n)
{
    fprintf(stderr, "✗ More than one exFAT volume is mounted; name the one to trim:\n");
    for (int i = 0; i < n; i++)
        fprintf(stderr, "    sudo %s \"%s\"\n", prog, names[i]);
}

void report_still_unmounted(const char *name, const char *bsd)
{
    fprintf(stderr, "  %s is still unmounted: run  diskutil mount %s  or replug it.\n", name, bsd);
}

void report_not_remounted(const char *name, const char *bsd, uint64_t trimmed)
{
    char h[32];
    fprintf(stderr, "✗ Trimmed %s, but %s did not mount again.\n", report_human(trimmed, h, sizeof h), name);
    fprintf(stderr, "  Run  diskutil mount %s  or replug the drive.\n", bsd);
}

static void format_elapsed(const struct timespec *t0, char *took, size_t n)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long secs = (long)(t1.tv_sec - t0->tv_sec);
    if (secs < 60) snprintf(took, n, "%lds", secs);
    else snprintf(took, n, "%ldm %02lds", secs / 60, secs % 60);
}

static void format_next_month(char *mon, size_t n, int *day)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_mon += 1;
    mktime(&tm);
    strftime(mon, n, "%b", &tm);
    *day = tm.tm_mday;
}

void report_dry_run(const char *name, uint64_t would_trim, uint64_t ranges, uint64_t free_bytes, uint64_t total_bytes)
{
    char h1[32], h2[32], h3[32];
    printf("%s (dry run): would trim %s in %llu range(s). Nothing changed.\n", name,
           report_human(would_trim, h3, sizeof h3), (unsigned long long)ranges);
    printf("  Free to use: %s of %s.\n", report_human(free_bytes, h1, sizeof h1),
           report_human(total_bytes, h2, sizeof h2));
}

void report_trimmed(const char *name, uint64_t trimmed, const struct timespec *started, uint64_t free_bytes,
                    uint64_t total_bytes, int mounted)
{
    char h1[32], h2[32], h3[32], took[32], mon[8];
    int day;
    format_elapsed(started, took, sizeof took);
    format_next_month(mon, sizeof mon, &day);
    printf("✓ %s trimmed: %s of free space handed back to the SSD (%s).\n", name,
           report_human(trimmed, h3, sizeof h3), took);
    printf("  Free to use: %s of %s. %s Next trim: ~%s %d.\n", report_human(free_bytes, h1, sizeof h1),
           report_human(total_bytes, h2, sizeof h2), mounted ? "Ready to use." : "Left unmounted, as it was.", mon, day);
}

void report_dry_sample(uint32_t ndirty, uint32_t draws)
{
    if (draws == 0) {
        printf("  Leftovers: no free clusters to sample.\n");
        return;
    }
    printf("  Leftovers: %u of %u sampled free clusters (%.0f%%).\n", ndirty, draws, 100.0 * ndirty / draws);
}

void report_verified(uint32_t draws, uint32_t ndirty, uint32_t cleared, double bound)
{
    if (ndirty == 0) {
        if (draws == 0) printf("  Verified: no free clusters to sample.\n");
        else printf("  Verified: all %u sampled free clusters already read as zeros.\n", draws);
        return;
    }
    printf("  Verified: %u of %u sampled leftovers now read as zeros (≥%.1f%% cleared, 95%% confidence).\n", cleared,
           ndirty, floor(bound * 1000.0) / 10.0);
}

void report_not_confirmed(uint32_t cleared, uint32_t ndirty)
{
    fflush(stdout);
    fprintf(stderr,
            "✗ Trim not confirmed: %u of %u sampled leftovers read as zeros afterwards. "
            "Either the drive ignored TRIM or it does not return zeros for trimmed blocks.\n",
            cleared, ndirty);
}

static int marker_path(char *path, size_t cap)
{
    const char *dir = getenv("EXFATRIM_TEST_LECTURE_DIR");
    if (!dir || !*dir) dir = LECTURE_DIR;
    const char *user = getenv("SUDO_USER");
    if (!user || !*user) {
        struct passwd *pw = getpwuid(getuid());
        user = pw ? pw->pw_name : NULL;
    }
    if (!user || !*user || strchr(user, '/') || !strcmp(user, ".") || !strcmp(user, "..")) return -1;
    return snprintf(path, cap, "%s/%s", dir, user) < (int)cap ? 0 : -1;
}

static int make_dirs(const char *path)
{
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return -1;
    for (char *p = buf + 1;; p++) {
        if (*p != '/' && *p != 0) continue;
        char sep = *p;
        *p = 0;
        if (mkdir(buf, 0755) == 0) chmod(buf, 0755);
        else if (errno != EEXIST) return -1;
        *p = sep;
        if (!sep) return 0;
    }
}

static int write_marker(const char *path)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (make_dirs(dir) != 0) return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return -1;
    return close(fd);
}

static int answered_yes(void)
{
    char line[64];
    if (!fgets(line, sizeof line, stdin)) return 0;
    size_t n = strcspn(line, "\r\n");
    line[n] = 0;
    return (line[0] == 'y' || line[0] == 'Y') && line[1] == 0;
}

enum report_confirm report_confirm_first_trim(const char *where)
{
    char path[PATH_MAX];
    int have_path = marker_path(path, sizeof path) == 0;
    if (have_path && access(path, F_OK) == 0) return REPORT_CONFIRM_YES;
    if (!isatty(STDIN_FILENO)) return REPORT_CONFIRM_NO_TTY;

    fprintf(stderr,
            "exfatrim clears the deleted files on %s.\n"
            "Your files and folders will not be changed.  After a trim undelete or recovery\n"
            "tools will no longer bring back any deleted file.\n"
            "\n"
            "If you are trying to recover deleted files from this volume STOP HERE.\n"
            "\n"
            "Continue? [y/N]\n",
            where);
    fflush(stderr);
    if (!answered_yes()) return REPORT_CONFIRM_NO;

    if (!have_path || write_marker(path) != 0)
        fprintf(stderr, "Couldn't record the confirmation (%s); the notice will show again next run.\n",
                have_path ? strerror(errno) : "no user name");
    return REPORT_CONFIRM_YES;
}
