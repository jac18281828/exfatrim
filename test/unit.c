#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/exfat.h"
#include "../src/verify.h"
#include "../src/walk.h"

#define BPS 512
#define NCLUS 9000
#define FAT_SECTOR 24
#define FAT_SECTORS 71
#define HEAP_SECTOR 96
#define VOL_SECTORS (HEAP_SECTOR + NCLUS)
#define IMG_BYTES ((size_t)VOL_SECTORS * BPS)
#define BITMAP_BYTES ((NCLUS + 7) / 8)
#define CL_BITMAP 2
#define CL_BITMAP_LAST 4
#define CL_UPCASE 5
#define CL_ROOT 6
#define CL_FILE 7
#define CL_FILE_LAST 9
#define FREE_FIRST_INDEX 8
#define ROOT_LABEL 0
#define ROOT_BITMAP 1
#define ROOT_UPCASE 2
#define ROOT_SPARE 3
#define NAME "TESTVOL"

static int g_failed;
static int g_tests;
static int g_tests_failed;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "  %s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failed++;                                                                \
    }                                                                            \
  } while (0)

#define REQUIRE(cond)                                                                     \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      fprintf(stderr, "  %s:%d: required check failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failed++;                                                                         \
      return;                                                                             \
    }                                                                                     \
  } while (0)

static void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t* p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint8_t* cluster_at(uint8_t* img, uint32_t c) { return img + (size_t)HEAP_SECTOR * BPS + (size_t)(c - 2) * BPS; }

static uint8_t* fat_at(uint8_t* img, uint32_t c) { return img + (size_t)FAT_SECTOR * BPS + (size_t)c * 4; }

static uint8_t* root_entry(uint8_t* img, unsigned i) { return cluster_at(img, CL_ROOT) + 32u * i; }

static void fix_checksum(uint8_t* img) {
  uint32_t sum = 0;
  for (size_t i = 0; i < 11 * (size_t)BPS; i++) {
    if (i == 106 || i == 107 || i == 112)
      continue;
    sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + img[i];
  }
  for (size_t i = 11 * (size_t)BPS; i < 12 * (size_t)BPS; i += 4)
    put32(img + i, sum);
}

static void clear_bitmap_bit(uint8_t* img, uint32_t cluster) {
  uint32_t i = cluster - 2;
  cluster_at(img, CL_BITMAP)[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static uint8_t* build_image(void) {
  uint8_t* img = calloc(IMG_BYTES, 1);
  if (!img) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  memcpy(img + 3, "EXFAT   ", 8);
  put64(img + 72, VOL_SECTORS);
  put32(img + 80, FAT_SECTOR);
  put32(img + 84, FAT_SECTORS);
  put32(img + 88, HEAP_SECTOR);
  put32(img + 92, NCLUS);
  put32(img + 96, CL_ROOT);
  img[105] = 1;
  img[108] = 9;
  img[109] = 0;
  img[110] = 1;
  img[510] = 0x55;
  img[511] = 0xAA;
  fix_checksum(img);

  put32(fat_at(img, 0), 0xFFFFFFF8u);
  put32(fat_at(img, 1), 0xFFFFFFFFu);
  for (uint32_t c = CL_BITMAP; c < CL_BITMAP_LAST; c++)
    put32(fat_at(img, c), c + 1);
  put32(fat_at(img, CL_BITMAP_LAST), 0xFFFFFFFFu);
  put32(fat_at(img, CL_UPCASE), 0xFFFFFFFFu);
  put32(fat_at(img, CL_ROOT), 0xFFFFFFFFu);
  for (uint32_t c = CL_FILE; c < CL_FILE_LAST; c++)
    put32(fat_at(img, c), c + 1);
  put32(fat_at(img, CL_FILE_LAST), 0xFFFFFFFFu);

  uint8_t* bitmap = cluster_at(img, CL_BITMAP);
  for (uint32_t c = CL_BITMAP; c <= CL_FILE_LAST; c++)
    bitmap[(c - 2) >> 3] |= (uint8_t)(1u << ((c - 2) & 7));

  uint8_t* label = root_entry(img, ROOT_LABEL);
  label[0] = 0x83;
  label[1] = (uint8_t)strlen(NAME);
  for (size_t i = 0; i < strlen(NAME); i++)
    put16(label + 2 + 2 * i, (uint16_t)NAME[i]);
  uint8_t* bm = root_entry(img, ROOT_BITMAP);
  bm[0] = 0x81;
  put32(bm + 20, CL_BITMAP);
  put64(bm + 24, BITMAP_BYTES);
  uint8_t* up = root_entry(img, ROOT_UPCASE);
  up[0] = 0x82;
  put32(up + 20, CL_UPCASE);
  put64(up + 24, 128);
  return img;
}

struct memdev {
  uint8_t* bytes;
  size_t size;
  unsigned reads;
  unsigned fail_at;
};

static int mem_read(void* ctx, uint64_t off, void* dst, size_t len) {
  struct memdev* m = ctx;
  m->reads++;
  if (m->fail_at && m->reads == m->fail_at)
    return -1;
  if (off + len < off || off + len > m->size)
    return -1;
  memcpy(dst, m->bytes + off, len);
  return 0;
}

struct loaded {
  enum exfat_status status;
  char reason[256];
  struct exfat_volume v;
};

static void load_with(uint8_t* img, size_t size, int allow_dirty, unsigned fail_at, struct loaded* out) {
  struct memdev m = {img, IMG_BYTES, 0, fail_at};
  struct exfat_source src = {mem_read, &m, size, NAME};
  out->status = exfat_load(&src, allow_dirty, &out->v, out->reason, sizeof out->reason);
}

static void load(uint8_t* img, int allow_dirty, struct loaded* out) { load_with(img, IMG_BYTES, allow_dirty, 0, out); }

static void test_valid_volume(void) {
  uint8_t* img = build_image();
  struct loaded l;
  load(img, 0, &l);
  REQUIRE(l.status == EXFAT_OK);
  CHECK(l.v.fs.nclus == NCLUS);
  CHECK(l.v.fs.clus == BPS);
  CHECK(l.v.fs.bps == BPS);
  CHECK(l.v.fs.heap_off == (uint64_t)HEAP_SECTOR * BPS);
  CHECK(l.v.fs.root == CL_ROOT);
  CHECK(l.v.meta.bitmap_first == CL_BITMAP);
  CHECK(l.v.meta.bitmap_len == BITMAP_BYTES);
  CHECK(l.v.meta.upcase_first == CL_UPCASE);
  CHECK(strcmp(l.v.meta.label, NAME) == 0);
  CHECK(l.v.meta.root_n == 1);
  CHECK(l.v.bitmap_n == 3);
  CHECK(l.v.total_bytes == (uint64_t)NCLUS * BPS);
  CHECK(l.v.free_bytes == (uint64_t)(NCLUS - FREE_FIRST_INDEX) * BPS);
  CHECK(!exfat_index_free(l.v.bitmap, FREE_FIRST_INDEX - 1));
  CHECK(exfat_index_free(l.v.bitmap, FREE_FIRST_INDEX));
  CHECK(exfat_cluster_allocated(l.v.bitmap, CL_ROOT));
  exfat_release(&l.v);
  free(img);
}

static void test_dirty_flag_allowed_when_asked(void) {
  uint8_t* img = build_image();
  put16(img + 106, 2);
  struct loaded l;
  load(img, 1, &l);
  CHECK(l.status == EXFAT_OK);
  exfat_release(&l.v);
  free(img);
}

static void m_bad_signature(uint8_t* img) { img[3] = 'X'; }

static void m_must_be_zero(uint8_t* img) { img[20] = 1; }

static void m_boot_signature(uint8_t* img) { img[511] = 0; }

static void m_sector_shift(uint8_t* img) { img[108] = 13; }

static void m_cluster_shift(uint8_t* img) { img[109] = 17; }

static void m_texfat(uint8_t* img) { img[110] = 2; }

static void m_bad_checksum(uint8_t* img) { put32(img + 11 * BPS, get32(img + 11 * BPS) ^ 1u); }

static void m_checksum_covers_boot(uint8_t* img) { img[200] = 0x5A; }

static void m_revision2(uint8_t* img) {
  img[105] = 2;
  fix_checksum(img);
}

static void m_media_failure(uint8_t* img) { put16(img + 106, 4); }

static void m_fat_offset_below_24(uint8_t* img) {
  put32(img + 80, 8);
  fix_checksum(img);
}

static void m_heap_overlaps_fat(uint8_t* img) {
  put32(img + 88, FAT_SECTOR + FAT_SECTORS - 1);
  fix_checksum(img);
}

static void m_zero_clusters(uint8_t* img) {
  put32(img + 92, 0);
  fix_checksum(img);
}

static void m_fat_too_short(uint8_t* img) {
  put32(img + 84, FAT_SECTORS - 1);
  fix_checksum(img);
}

static void m_volume_past_device(uint8_t* img) {
  put64(img + 72, VOL_SECTORS + 1);
  fix_checksum(img);
}

static void m_heap_past_volume(uint8_t* img) {
  put64(img + 72, VOL_SECTORS - 1);
  fix_checksum(img);
}

static void m_root_outside(uint8_t* img) {
  put32(img + 96, 1);
  fix_checksum(img);
}

static void m_dirty(uint8_t* img) { put16(img + 106, 2); }

static void m_bitmap_len_plus_one(uint8_t* img) { put64(root_entry(img, ROOT_BITMAP) + 24, BITMAP_BYTES + 1); }

static void m_bitmap_len_minus_one(uint8_t* img) { put64(root_entry(img, ROOT_BITMAP) + 24, BITMAP_BYTES - 1); }

static void m_bitmap_self_loop(uint8_t* img) { put32(fat_at(img, CL_BITMAP), CL_BITMAP); }

static void m_bitmap_two_cluster_loop(uint8_t* img) { put32(fat_at(img, CL_BITMAP + 1), CL_BITMAP); }

static void m_bitmap_chain_leaves_heap(uint8_t* img) { put32(fat_at(img, CL_BITMAP + 1), 100000); }

static void m_bitmap_chain_below_heap(uint8_t* img) { put32(fat_at(img, CL_BITMAP + 1), 1); }

static void m_bitmap_chain_ends_early(uint8_t* img) { put32(fat_at(img, CL_BITMAP + 1), 0xFFFFFFFFu); }

static void m_root_self_loop(uint8_t* img) { put32(fat_at(img, CL_ROOT), CL_ROOT); }

static void m_two_bitmaps(uint8_t* img) { memcpy(root_entry(img, ROOT_SPARE), root_entry(img, ROOT_BITMAP), 32); }

static void m_no_bitmap(uint8_t* img) { root_entry(img, ROOT_BITMAP)[0] = 0x01; }

static void m_root_marked_free(uint8_t* img) { clear_bitmap_bit(img, CL_ROOT); }

static void m_bitmap_marked_free(uint8_t* img) { clear_bitmap_bit(img, CL_BITMAP + 1); }

static void m_upcase_marked_free(uint8_t* img) { clear_bitmap_bit(img, CL_UPCASE); }

struct refusal {
  const char* name;
  void (*mutate)(uint8_t* img);
  enum exfat_status status;
  const char* text;
};

static const struct refusal refusals[] = {
    {"bad signature", m_bad_signature, EXFAT_E_NOT_EXFAT, "isn't an exFAT volume"},
    {"nonzero must-be-zero region", m_must_be_zero, EXFAT_E_BOOT_MALFORMED, "malformed exFAT boot sector"},
    {"bad boot signature", m_boot_signature, EXFAT_E_BOOT_MALFORMED, "malformed exFAT boot sector"},
    {"sector shift out of range", m_sector_shift, EXFAT_E_BOOT_MALFORMED, "malformed exFAT boot sector"},
    {"cluster shift out of range", m_cluster_shift, EXFAT_E_BOOT_MALFORMED, "malformed exFAT boot sector"},
    {"two FATs (TexFAT)", m_texfat, EXFAT_E_TEXFAT, "TexFAT"},
    {"bad boot checksum", m_bad_checksum, EXFAT_E_CHECKSUM, "boot checksum doesn't match"},
    {"boot sector edited without its checksum", m_checksum_covers_boot, EXFAT_E_CHECKSUM,
     "boot checksum doesn't match"},
    {"revision 2", m_revision2, EXFAT_E_REVISION, "exFAT revision 2.0"},
    {"media failure flag", m_media_failure, EXFAT_E_MEDIA_FAILURE, "media failure"},
    {"FAT offset below 24 sectors", m_fat_offset_below_24, EXFAT_E_FAT_OFFSET, "geometry doesn't fit"},
    {"heap overlapping the FAT", m_heap_overlaps_fat, EXFAT_E_HEAP_OVERLAPS_FAT, "geometry doesn't fit"},
    {"zero clusters", m_zero_clusters, EXFAT_E_GEOMETRY, "geometry doesn't fit"},
    {"FAT shorter than the cluster count", m_fat_too_short, EXFAT_E_GEOMETRY, "geometry doesn't fit"},
    {"volume longer than the device", m_volume_past_device, EXFAT_E_GEOMETRY, "geometry doesn't fit"},
    {"heap past the end of the volume", m_heap_past_volume, EXFAT_E_GEOMETRY, "geometry doesn't fit"},
    {"root cluster outside the heap", m_root_outside, EXFAT_E_GEOMETRY, "geometry doesn't fit"},
    {"dirty flag", m_dirty, EXFAT_E_DIRTY, "wasn't closed cleanly"},
    {"bitmap length one too long", m_bitmap_len_plus_one, EXFAT_E_BITMAP_LENGTH, "allocation bitmap's length"},
    {"bitmap length one too short", m_bitmap_len_minus_one, EXFAT_E_BITMAP_LENGTH, "allocation bitmap's length"},
    {"self-loop in the bitmap chain", m_bitmap_self_loop, EXFAT_E_CHAIN_LOOP, "cluster chain loops"},
    {"two-cluster loop in the bitmap chain", m_bitmap_two_cluster_loop, EXFAT_E_CHAIN_LOOP, "cluster chain loops"},
    {"bitmap chain leaving the heap", m_bitmap_chain_leaves_heap, EXFAT_E_CHAIN_OUTSIDE, "outside the volume"},
    {"bitmap chain below the heap", m_bitmap_chain_below_heap, EXFAT_E_CHAIN_OUTSIDE, "outside the volume"},
    {"bitmap chain ending early", m_bitmap_chain_ends_early, EXFAT_E_CHAIN_EARLY_END, "chain ends early"},
    {"root directory chain loops", m_root_self_loop, EXFAT_E_CHAIN_LOOP, "root directory's cluster chain loops"},
    {"two bitmaps", m_two_bitmaps, EXFAT_E_BITMAP_COUNT, "found 2 allocation bitmaps"},
    {"no bitmap", m_no_bitmap, EXFAT_E_BITMAP_COUNT, "found 0 allocation bitmaps"},
    {"root directory marked free", m_root_marked_free, EXFAT_E_ROOT_FREE, "root directory sits in space marked free"},
    {"bitmap marked free", m_bitmap_marked_free, EXFAT_E_BITMAP_FREE, "allocation bitmap sits in space marked free"},
    {"up-case table marked free", m_upcase_marked_free, EXFAT_E_UPCASE_FREE, "up-case table sits in space marked free"},
};

static void test_refusal(const void* arg) {
  const struct refusal* r = arg;
  uint8_t* img = build_image();
  r->mutate(img);
  struct loaded l;
  load(img, 0, &l);
  CHECK(l.status == r->status);
  CHECK(strstr(l.reason, r->text) != NULL);
  if (strstr(l.reason, r->text) == NULL)
    fprintf(stderr, "  reason was: %s\n", l.reason);
  CHECK(l.v.bitmap == NULL);
  CHECK(l.v.bitmap_chain == NULL);
  CHECK(l.v.meta.root_chain == NULL);
  free(img);
}

static void test_short_device(void) {
  uint8_t* img = build_image();
  struct loaded l;
  load_with(img, IMG_BYTES - BPS, 0, 0, &l);
  CHECK(l.status == EXFAT_E_GEOMETRY);
  free(img);
}

static void test_read_failures(void) {
  uint8_t* img = build_image();
  struct memdev count = {img, IMG_BYTES, 0, 0};
  struct exfat_source counting = {mem_read, &count, IMG_BYTES, NAME};
  struct exfat_volume v;
  char reason[256];
  CHECK(exfat_load(&counting, 0, &v, reason, sizeof reason) == EXFAT_OK);
  exfat_release(&v);
  CHECK(count.reads >= 6);
  struct loaded l;
  for (unsigned at = 1; at <= count.reads; at++) {
    load_with(img, IMG_BYTES, 0, at, &l);
    CHECK(l.status == EXFAT_E_READ);
    CHECK(l.reason[0] != 0);
    CHECK(l.v.bitmap == NULL);
  }
  free(img);
}

static void set_bit(uint8_t* bm, uint32_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static void fill_allocated(uint8_t* bm, uint32_t from, uint32_t to) {
  for (uint32_t i = from; i < to; i++)
    set_bit(bm, i);
}

struct walk_case {
  struct exfat_fs fs;
  uint8_t bitmap[64];
};

static void walk_case_init(struct walk_case* w, uint64_t clus, uint32_t nclus, uint64_t heap_off) {
  memset(w, 0, sizeof *w);
  w->fs.bps = 512;
  w->fs.clus = clus;
  w->fs.nclus = nclus;
  w->fs.heap_off = heap_off;
}

static enum walk_status walk(struct walk_case* w, uint64_t align, struct walk_plan* p) {
  char reason[128];
  return walk_runs(&w->fs, w->bitmap, align, p, reason, sizeof reason);
}

static void test_walk_every_cluster_free(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == 12288 && p.v[0].e == 12288 + 64 * 512);
  CHECK(p.v[0].first == 0 && p.v[0].n == 64);
  CHECK(p.bytes == 64 * 512);
  CHECK(p.population == 64);
  walk_release(&p);
}

static void test_walk_no_free_run(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  fill_allocated(w.bitmap, 0, 64);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  CHECK(p.n == 0 && p.bytes == 0 && p.population == 0);
  walk_release(&p);
}

static void test_walk_run_shorter_than_unit(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  fill_allocated(w.bitmap, 0, 3);
  fill_allocated(w.bitmap, 8, 20);
  fill_allocated(w.bitmap, 23, 64);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  CHECK(p.n == 0);
  CHECK(p.bytes == 0);
  CHECK(p.population == 0);
  walk_release(&p);
}

static void test_walk_shrinks_inward(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  fill_allocated(w.bitmap, 0, 5);
  fill_allocated(w.bitmap, 31, 64);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == 16384);
  CHECK(p.v[0].e == 24576);
  CHECK(p.v[0].first == 8);
  CHECK(p.v[0].n == 16);
  CHECK(p.bytes == 8192);
  CHECK(p.population == 16);
  walk_release(&p);
}

static void test_walk_run_touching_heap_end(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  fill_allocated(w.bitmap, 0, 40);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == 32768);
  CHECK(p.v[0].e == 12288 + 64 * 512);
  walk_release(&p);
}

static void test_walk_unaligned_heap_end(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 61, 12288);
  fill_allocated(w.bitmap, 0, 40);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == 32768);
  CHECK(p.v[0].e == 40960);
  CHECK(p.v[0].e <= w.fs.heap_off + 61 * 512);
  walk_release(&p);
}

static void test_walk_clusters_smaller_than_unit(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  fill_allocated(w.bitmap, 0, 1);
  fill_allocated(w.bitmap, 2, 5);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == 16384);
  CHECK(p.v[0].e == 12288 + 64 * 512);
  CHECK(p.v[0].first == 8);
  walk_release(&p);
}

static void test_walk_clusters_larger_than_unit(void) {
  struct walk_case w;
  walk_case_init(&w, 4096, 32, 8192);
  fill_allocated(w.bitmap, 0, 3);
  fill_allocated(w.bitmap, 6, 32);
  struct walk_plan p;
  CHECK(walk(&w, 512, &p) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == 8192 + 3 * 4096);
  CHECK(p.v[0].e == 8192 + 6 * 4096);
  CHECK(p.v[0].first == 3 && p.v[0].n == 3);
  walk_release(&p);
}

static void test_walk_population_prefix(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  fill_allocated(w.bitmap, 0, 4);
  fill_allocated(w.bitmap, 16, 24);
  fill_allocated(w.bitmap, 40, 64);
  struct walk_plan p;
  CHECK(walk(&w, 4096, &p) == WALK_OK);
  REQUIRE(p.n == 2);
  CHECK(p.v[0].first == 8 && p.v[0].n == 8 && p.v[0].before == 0);
  CHECK(p.v[1].first == 24 && p.v[1].n == 16 && p.v[1].before == 8);
  CHECK(p.population == 24);
  walk_release(&p);
}

static void test_walk_zero_alignment(void) {
  struct walk_case w;
  walk_case_init(&w, 512, 64, 12288);
  struct walk_plan p;
  CHECK(walk(&w, 0, &p) == WALK_E_ALIGN);
  CHECK(p.v == NULL);
}

static void test_walk_over_a_parsed_volume(void) {
  uint8_t* img = build_image();
  struct loaded l;
  load(img, 0, &l);
  CHECK(l.status == EXFAT_OK);
  struct walk_plan p;
  char reason[128];
  CHECK(walk_runs(&l.v.fs, l.v.bitmap, 4096, &p, reason, sizeof reason) == WALK_OK);
  REQUIRE(p.n == 1);
  CHECK(p.v[0].s == (uint64_t)(HEAP_SECTOR + FREE_FIRST_INDEX) * BPS);
  CHECK(p.v[0].e == (uint64_t)VOL_SECTORS * BPS);
  walk_release(&p);
  exfat_release(&l.v);
  free(img);
}

static void test_sample_size_follows_confidence(void) {
  CHECK(verify_sample_size(50.0) == 69);
  CHECK(verify_sample_size(60.0) == 92);
  CHECK(verify_sample_size(VERIFY_CONFIDENCE_DEFAULT) == 299);
  CHECK(verify_sample_size(99.0) == 459);
  CHECK(verify_sample_size(VERIFY_CONFIDENCE_MAX) == 688);
  CHECK(verify_sample_size(VERIFY_CONFIDENCE_MAX) == VERIFY_SAMPLE_MAX);
}

static void test_confidence_argument(void) {
  static const char* const good[] = {"50", "50.0", "60", "95", "99", "99.9", "99.90", "75.5"};
  static const char* const bad[] = {"",    "49.9", "49",  "100", "99.91", "99.95", "0",  "abc", "99x", "-60", "+60",
                                    "1e2", "0x60", " 99", "99 ", ".",     "9.9.",  "..", "inf", "nan", "9,9", "99.9.9"};
  double pct = 0;
  for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
    pct = 0;
    CHECK(verify_parse_confidence(good[i], &pct) == 0);
    CHECK(pct >= VERIFY_CONFIDENCE_MIN && pct <= VERIFY_CONFIDENCE_MAX);
  }
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    CHECK(verify_parse_confidence(bad[i], &pct) != 0);
  CHECK(verify_parse_confidence("99.9", &pct) == 0 && pct == 99.9);
}

static void finish(const char* name) {
  g_tests++;
  if (g_failed)
    g_tests_failed++;
  printf("%s unit: %s\n", g_failed ? "FAIL" : "PASS", name);
  g_failed = 0;
}

#define RUN(fn)  \
  do {           \
    fn();        \
    finish(#fn); \
  } while (0)

int main(void) {
  RUN(test_valid_volume);
  RUN(test_dirty_flag_allowed_when_asked);
  for (size_t i = 0; i < sizeof refusals / sizeof refusals[0]; i++) {
    test_refusal(&refusals[i]);
    char name[96];
    snprintf(name, sizeof name, "refuses %s", refusals[i].name);
    finish(name);
  }
  RUN(test_short_device);
  RUN(test_read_failures);
  RUN(test_walk_every_cluster_free);
  RUN(test_walk_no_free_run);
  RUN(test_walk_run_shorter_than_unit);
  RUN(test_walk_shrinks_inward);
  RUN(test_walk_run_touching_heap_end);
  RUN(test_walk_unaligned_heap_end);
  RUN(test_walk_clusters_smaller_than_unit);
  RUN(test_walk_clusters_larger_than_unit);
  RUN(test_walk_population_prefix);
  RUN(test_walk_zero_alignment);
  RUN(test_walk_over_a_parsed_volume);
  RUN(test_sample_size_follows_confidence);
  RUN(test_confidence_argument);
  printf("UNIT %s: %d tests, %d failed\n", g_tests_failed ? "FAIL" : "PASS", g_tests, g_tests_failed);
  return g_tests_failed ? 1 : 0;
}
