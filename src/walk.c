#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "walk.h"

static uint64_t align_up(uint64_t x, uint64_t align) { return (x + align - 1) / align * align; }

static uint64_t align_down(uint64_t x, uint64_t align) { return x / align * align; }

static enum walk_status say(char* reason, size_t cap, enum walk_status st, const char* text) {
  if (reason && cap)
    snprintf(reason, cap, "%s", text);
  return st;
}

static enum walk_status plan_add(struct walk_plan* p, const struct exfat_fs* f, uint64_t s, uint64_t e, char* reason,
                                 size_t cap) {
  uint64_t heap_end = f->heap_off + (uint64_t)f->nclus * f->clus;
  if (s < f->heap_off || e > heap_end)
    return say(reason, cap, WALK_E_OUTSIDE, "Internal error: extent outside the data area. Stopped.");
  if (p->n == p->cap) {
    size_t grown = p->cap ? p->cap * 2 : 1024;
    struct walk_run* v = realloc(p->v, grown * sizeof *v);
    if (!v)
      return say(reason, cap, WALK_E_NOMEM, "Out of memory.");
    p->v = v;
    p->cap = grown;
  }
  uint64_t c0 = (s - f->heap_off + f->clus - 1) / f->clus;
  uint64_t c1 = (e - f->heap_off) / f->clus;
  struct walk_run* r = &p->v[p->n++];
  r->s = s;
  r->e = e;
  r->first = (uint32_t)c0;
  r->n = c1 > c0 ? (uint32_t)(c1 - c0) : 0;
  r->before = p->population;
  p->population += r->n;
  p->bytes += e - s;
  return WALK_OK;
}

enum walk_status walk_runs(const struct exfat_fs* f, const uint8_t* bitmap, uint64_t align, struct walk_plan* p,
                           char* reason, size_t cap) {
  memset(p, 0, sizeof *p);
  if (reason && cap)
    reason[0] = 0;
  if (align == 0)
    return say(reason, cap, WALK_E_ALIGN, "Internal error: alignment unit is zero. Stopped.");
  for (uint32_t i = 0; i < f->nclus;) {
    if (bitmap[i >> 3] == 0xFF && (i & 7) == 0) {
      i += 8;
      continue;
    }
    if (!exfat_index_free(bitmap, i)) {
      i++;
      continue;
    }
    uint32_t j = i;
    while (j < f->nclus && exfat_index_free(bitmap, j))
      j++;
    uint64_t s = align_up(exfat_cluster_offset(f, i + 2), align);
    uint64_t e = align_down(exfat_cluster_offset(f, j + 2), align);
    if (s < e) {
      enum walk_status st = plan_add(p, f, s, e, reason, cap);
      if (st != WALK_OK) {
        walk_release(p);
        return st;
      }
    }
    i = j;
  }
  return WALK_OK;
}

void walk_release(struct walk_plan* p) {
  free(p->v);
  memset(p, 0, sizeof *p);
}
