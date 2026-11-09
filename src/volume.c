#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include "report.h"
#include "shell.h"
#include "volume.h"

extern char** environ;

struct volume g_volume;
volatile sig_atomic_t g_stop;

static int run_quiet(const char* path, char* const argv[]) {
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_init(&attr);
  posix_spawnattr_setpgroup(&attr, 0);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  pid_t pid;
  int rc = posix_spawn(&pid, path, &fa, &attr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0)
    return -1;
  int st;
  while (waitpid(pid, &st, 0) < 0)
    if (errno != EINTR)
      return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void volume_remount(void) {
  if (!g_volume.owe_remount)
    return;
  char dev[80];
  snprintf(dev, sizeof dev, "/dev/%s", g_volume.bsd);
  char* argv[] = {"diskutil", "mount", dev, NULL};
  if (run_quiet("/usr/sbin/diskutil", argv) == 0)
    g_volume.owe_remount = 0;
}

void die(const char* fmt, ...) {
  if (g_volume.device)
    device_close(g_volume.device);
  report_clear_status();
  volume_remount();
  va_list ap;
  va_start(ap, fmt);
  report_errorv(fmt, ap);
  va_end(ap);
  if (g_volume.owe_remount) {
    report_still_unmounted(g_volume.name, g_volume.bsd);
    exit(EXIT_NOT_REMOUNTED);
  }
  exit(EXIT_REFUSED);
}

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

void volume_install_signals(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);
}

static int is_bsd_id(const char* s) {
  if (strncmp(s, "disk", 4))
    return 0;
  const char* p = s + 4;
  if (!(*p >= '0' && *p <= '9'))
    return 0;
  while (*p >= '0' && *p <= '9')
    p++;
  if (*p == 's') {
    p++;
    if (!(*p >= '0' && *p <= '9'))
      return 0;
    while (*p >= '0' && *p <= '9')
      p++;
  }
  return *p == 0;
}

static const char* strip_from(const char* from) { return strncmp(from, "/dev/", 5) ? from : from + 5; }

static void adopt_mount(const struct statfs* m) {
  if (strcmp(m->f_fstypename, "exfat"))
    die("%s is %s, not exFAT.", m->f_mntonname, m->f_fstypename);
  snprintf(g_volume.bsd, sizeof g_volume.bsd, "%s", strip_from(m->f_mntfromname));
  snprintf(g_volume.name, sizeof g_volume.name, "%s", strrchr(m->f_mntonname, '/') + 1);
  snprintf(g_volume.where, sizeof g_volume.where, "%s", m->f_mntonname);
  g_volume.mounted = 1;
}

void volume_read_free_space(void) {
  if (!g_volume.mounted)
    return;
  struct statfs st;
  sync();
  if (statfs(g_volume.where, &st) < 0)
    die("Can't read free space on %s: %s", g_volume.name, strerror(errno));
  g_volume.df_free = (uint64_t)st.f_bavail * st.f_bsize;
}

void volume_resolve(const char* arg) {
  struct statfs* mnt;
  int n = getmntinfo(&mnt, MNT_NOWAIT);
  g_volume.mounted = 0;

  if (!arg) {
    int hits = 0, pick = -1;
    for (int i = 0; i < n; i++)
      if (!strcmp(mnt[i].f_fstypename, "exfat")) {
        hits++;
        pick = i;
      }
    if (hits == 0)
      die("No exFAT volume is mounted. Plug in the drive, or name it: sudo %s SSD", g_volume.prog);
    if (hits > 1) {
      const char** names = calloc((size_t)hits, sizeof *names);
      if (!names)
        die("Out of memory.");
      int k = 0;
      for (int i = 0; i < n; i++)
        if (!strcmp(mnt[i].f_fstypename, "exfat"))
          names[k++] = strrchr(mnt[i].f_mntonname, '/') + 1;
      report_choose_volume(g_volume.prog, names, k);
      exit(EXIT_REFUSED);
    }
    adopt_mount(&mnt[pick]);
    return;
  }

  char want_path[1100];
  if (arg[0] == '/')
    snprintf(want_path, sizeof want_path, "%s", arg);
  else
    snprintf(want_path, sizeof want_path, "/Volumes/%s", arg);
  for (int i = 0; i < n; i++)
    if (!strcmp(mnt[i].f_mntonname, want_path)) {
      adopt_mount(&mnt[i]);
      return;
    }

  const char* id = arg;
  if (!strncmp(id, "/dev/", 5))
    id += 5;
  if (!strncmp(id, "rdisk", 5))
    id += 1;
  if (!is_bsd_id(id))
    die("Can't find a mounted volume called \"%s\".", arg);
  for (int i = 0; i < n; i++)
    if (!strcmp(strip_from(mnt[i].f_mntfromname), id)) {
      adopt_mount(&mnt[i]);
      return;
    }
  snprintf(g_volume.bsd, sizeof g_volume.bsd, "%s", id);
  snprintf(g_volume.name, sizeof g_volume.name, "%s", id);
  snprintf(g_volume.where, sizeof g_volume.where, "/dev/%s", id);
}

static int still_mounted(void) {
  struct statfs* mnt;
  int n = getmntinfo(&mnt, MNT_NOWAIT);
  for (int i = 0; i < n; i++)
    if (!strcmp(strip_from(mnt[i].f_mntfromname), g_volume.bsd))
      return 1;
  return 0;
}

static const char* command_as_typed(void) {
  char* line = shell_join(g_volume.argc, g_volume.argv);
  return line ? line : g_volume.prog;
}

void volume_require_access(const char* raw, int dry) {
  int open_errno = 0;
  switch (device_probe(raw, dry, &open_errno)) {
  case DEVICE_PROBE_OK:
    return;
  case DEVICE_PROBE_NEEDS_ADMIN:
    die("Needs administrator rights. Run:  sudo %s", command_as_typed());
  case DEVICE_PROBE_BUSY:
    die("Another process holds %s, so exfatrim can't lock it. Nothing changed.", raw);
  case DEVICE_PROBE_OPEN_FAILED:
    die("Can't open %s: %s", raw, strerror(open_errno));
  case DEVICE_PROBE_NO_UNMAP:
    die("macOS reports %s can't accept TRIM over this connection. Nothing changed.", g_volume.name);
  }
}

void volume_unmount(const char* dev) {
  report_status("Unmounting %s…", g_volume.name);
  char* um[] = {"diskutil", "unmount", (char*)dev, NULL};
  int rc = run_quiet("/usr/sbin/diskutil", um);
  int mounted_now = still_mounted();
  if (rc != 0 || mounted_now || g_stop) {
    g_volume.owe_remount = !mounted_now;
    if (g_stop)
      die("Stopped. Nothing was trimmed.");
    die("Couldn't unmount %s. Close anything using it (Finder windows, apps, Terminal tabs in it) and try again.",
        g_volume.name);
  }
  g_volume.owe_remount = 1;
}

void volume_check_filesystem(const char* raw) {
  const char* skip = getenv("EXFATRIM_TEST_SKIP_FSCK");
  if (skip && !strcmp(skip, "1"))
    return;
  report_status("Checking %s for errors…", g_volume.name);
  char* fsck[] = {"fsck_exfat", "-n", (char*)raw, NULL};
  int rc = run_quiet("/sbin/fsck_exfat", fsck);
  if (g_stop)
    die("Stopped. Nothing was trimmed.");
  if (rc != 0)
    die("%s has file-system errors (fsck_exfat exit %d). Run First Aid in Disk Utility, then try again. Nothing was trimmed.",
        g_volume.name, rc);
}
