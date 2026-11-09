#ifndef EXFATRIM_EXFAT_H
#define EXFATRIM_EXFAT_H

#include <stddef.h>
#include <stdint.h>

typedef int (*exfat_read_fn)(void* ctx, uint64_t offset, void* dst, size_t len);

struct exfat_source {
  exfat_read_fn read;
  void* ctx;
  uint64_t size;
  const char* name;
};

enum exfat_status {
  EXFAT_OK = 0,
  EXFAT_E_READ,
  EXFAT_E_NOMEM,
  EXFAT_E_NOT_EXFAT,
  EXFAT_E_BOOT_MALFORMED,
  EXFAT_E_TEXFAT,
  EXFAT_E_CHECKSUM,
  EXFAT_E_REVISION,
  EXFAT_E_MEDIA_FAILURE,
  EXFAT_E_FAT_OFFSET,
  EXFAT_E_HEAP_OVERLAPS_FAT,
  EXFAT_E_GEOMETRY,
  EXFAT_E_DIRTY,
  EXFAT_E_CHAIN_TOO_LONG,
  EXFAT_E_CHAIN_OUTSIDE,
  EXFAT_E_CHAIN_LOOP,
  EXFAT_E_CHAIN_EARLY_END,
  EXFAT_E_BITMAP_COUNT,
  EXFAT_E_BITMAP_LENGTH,
  EXFAT_E_BITMAP_SHORT,
  EXFAT_E_ROOT_FREE,
  EXFAT_E_BITMAP_FREE,
  EXFAT_E_UPCASE_FREE
};

struct exfat_fs {
  uint32_t bps;
  uint64_t clus;
  uint64_t fat_off;
  uint64_t fat_len;
  uint64_t heap_off;
  uint32_t nclus;
  uint32_t root;
  uint16_t flags;
};

struct exfat_meta {
  uint32_t bitmap_first;
  uint64_t bitmap_len;
  uint32_t upcase_first;
  uint32_t* root_chain;
  uint32_t root_n;
  char label[64];
};

struct exfat_volume {
  struct exfat_fs fs;
  struct exfat_meta meta;
  uint8_t* bitmap;
  uint32_t* bitmap_chain;
  uint32_t bitmap_n;
  uint64_t free_bytes;
  uint64_t total_bytes;
};

enum exfat_status exfat_load(const struct exfat_source* src, int allow_dirty, struct exfat_volume* v, char* reason,
                             size_t reason_cap);

void exfat_release(struct exfat_volume* v);

int exfat_valid_cluster(const struct exfat_fs* f, uint32_t c);
uint64_t exfat_cluster_offset(const struct exfat_fs* f, uint32_t c);
int exfat_cluster_allocated(const uint8_t* bitmap, uint32_t c);
int exfat_index_free(const uint8_t* bitmap, uint32_t index);

#endif
