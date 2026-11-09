#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "device.h"

enum device_probe device_probe(const char* raw, int dry, int* open_errno) {
  int probe = open(raw, dry ? O_RDONLY : O_RDWR);
  if (probe < 0) {
    *open_errno = errno;
    if (errno == EACCES || errno == EPERM)
      return DEVICE_PROBE_NEEDS_ADMIN;
    if (errno == EBUSY)
      return DEVICE_PROBE_BUSY;
    return DEVICE_PROBE_OPEN_FAILED;
  }
  uint32_t features = 0;
  int bad = ioctl(probe, DKIOCGETFEATURES, &features) < 0 || !(features & DK_FEATURE_UNMAP);
  close(probe);
  return bad ? DEVICE_PROBE_NO_UNMAP : DEVICE_PROBE_OK;
}

static int refuse(char* err, size_t cap, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
  return -1;
}

int device_open(struct device* d, const char* raw, int dry, const char* name, char* err, size_t err_cap) {
  d->name = name;
  if (dry) {
    d->fd = open(raw, O_RDONLY);
  } else {
    d->fd = open(raw, O_RDWR | O_EXLOCK | O_NONBLOCK);
    if (d->fd < 0 && (errno == EWOULDBLOCK || errno == EBUSY))
      return refuse(err, err_cap, "Another process holds %s, so exfatrim can't lock it. Nothing was trimmed.", raw);
    if (d->fd >= 0)
      fcntl(d->fd, F_SETFL, fcntl(d->fd, F_GETFL) & ~O_NONBLOCK);
  }
  if (d->fd < 0)
    return refuse(err, err_cap, "Can't open %s: %s", raw, strerror(errno));

  uint64_t nblk = 0;
  if (ioctl(d->fd, DKIOCGETBLOCKSIZE, &d->blk) < 0 || ioctl(d->fd, DKIOCGETBLOCKCOUNT, &nblk) < 0 || d->blk == 0)
    return refuse(err, err_cap, "Can't read %s's size: %s", raw, strerror(errno));
  d->size = nblk * d->blk;

  d->align = d->blk;
  const char* tb = getenv("EXFATRIM_TEST_BLOCK");
  if (tb) {
    char* end;
    unsigned long v = strtoul(tb, &end, 10);
    if (*end || v < d->blk || v > UINT32_MAX || (v & (v - 1)))
      return refuse(err, err_cap, "EXFATRIM_TEST_BLOCK must be a power of two of at least %u.", d->blk);
    d->align = (uint32_t)v;
  }
  return 0;
}

void device_close(struct device* d) {
  if (d->fd >= 0) {
    close(d->fd);
    d->fd = -1;
  }
}

int device_read(void* ctx, uint64_t off, void* dst, size_t len) {
  struct device* d = ctx;
  d->nomem = 0;
  if (len == 0)
    return 0;
  if (off + len < off || off + len > d->size) {
    snprintf(d->err, sizeof d->err, "Tried to read past the end of %s.", d->name);
    return -1;
  }
  uint64_t s = off / d->blk * d->blk;
  uint64_t e = (off + len + d->blk - 1) / d->blk * d->blk;
  size_t n = (size_t)(e - s);
  void* buf;
  if (posix_memalign(&buf, 4096, n)) {
    d->nomem = 1;
    snprintf(d->err, sizeof d->err, "Out of memory.");
    return -1;
  }
  for (size_t done = 0; done < n;) {
    size_t chunk = n - done > (1u << 20) ? (1u << 20) : n - done;
    ssize_t r = pread(d->fd, (char*)buf + done, chunk, (off_t)(s + done));
    if (r <= 0) {
      snprintf(d->err, sizeof d->err, "Read error on %s: %s.", d->name, r < 0 ? strerror(errno) : "short read");
      free(buf);
      return -1;
    }
    done += (size_t)r;
  }
  memcpy(dst, (char*)buf + (off - s), len);
  free(buf);
  return 0;
}

int device_unmap(struct device* d, const struct device_extent* ext, size_t n) {
  if (n > DEVICE_MAX_EXTENTS)
    return EINVAL;
  dk_extent_t ex[DEVICE_MAX_EXTENTS];
  for (size_t i = 0; i < n; i++) {
    ex[i].offset = ext[i].offset;
    ex[i].length = ext[i].length;
  }
  dk_unmap_t u;
  memset(&u, 0, sizeof u);
  u.extents = ex;
  u.extentsCount = (uint32_t)n;
  if (ioctl(d->fd, DKIOCUNMAP, &u) < 0)
    return errno;
  return 0;
}
