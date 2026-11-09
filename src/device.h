#ifndef EXFATRIM_DEVICE_H
#define EXFATRIM_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#define DEVICE_MAX_EXTENTS 64

struct device {
  int fd;
  uint32_t blk;
  uint32_t align;
  uint64_t size;
  const char* name;
  int nomem;
  char err[256];
};

struct device_extent {
  uint64_t offset;
  uint64_t length;
};

enum device_probe {
  DEVICE_PROBE_OK = 0,
  DEVICE_PROBE_NEEDS_ADMIN,
  DEVICE_PROBE_BUSY,
  DEVICE_PROBE_OPEN_FAILED,
  DEVICE_PROBE_NO_UNMAP
};

enum device_probe device_probe(const char* raw, int dry, int* open_errno);

int device_open(struct device* d, const char* raw, int dry, const char* name, char* err, size_t err_cap);

void device_close(struct device* d);

int device_read(void* ctx, uint64_t offset, void* dst, size_t len);

int device_unmap(struct device* d, const struct device_extent* ext, size_t n);

#endif
