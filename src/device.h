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

/*
 * Opens raw, queries DKIOCGETFEATURES for DK_FEATURE_UNMAP and closes it.
 * open_errno receives errno for DEVICE_PROBE_OPEN_FAILED.
 */
enum device_probe device_probe(const char* raw, int dry, int* open_errno);

/*
 * Opens raw read-only for a dry run, else read-write under an exclusive lock,
 * and reads the block size and count. EXFATRIM_TEST_BLOCK raises the
 * alignment unit. Returns 0, or -1 with the user-facing text in err.
 */
int device_open(struct device* d, const char* raw, int dry, const char* name, char* err, size_t err_cap);

void device_close(struct device* d);

/*
 * Reads len bytes at offset through block-aligned reads. Matches exfat_read_fn
 * with ctx a struct device. Returns 0, or -1 with the reason in d->err.
 */
int device_read(void* ctx, uint64_t offset, void* dst, size_t len);

/* Sends DKIOCUNMAP for up to DEVICE_MAX_EXTENTS extents. Returns 0 or errno. */
int device_unmap(struct device* d, const struct device_extent* ext, size_t n);

#endif
