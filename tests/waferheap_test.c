/* waferheap_test.c ... host functional test for WaferHeap.
 *
 * Native build and run (this file is a test, not part of the
 * library):
 *
 *   gcc -std=c99 -Wall -Wextra -I include tests/waferheap_test.c \
 *       src/waferheap.c -o /tmp/waferheap_test && /tmp/waferheap_test
 *
 * Method: a deterministic PRNG drives randomized alloc, free and
 * realloc churn against a shadow model. Every live allocation is
 * filled with a pattern derived from its slot index; patterns are
 * verified before frees, after reallocs and periodically across the
 * whole model, so any overlap, stomp or lost byte fails the case
 * that catches it. Prints PASS or FAIL per case and a final verdict.
 */

#include "waferheap.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CASE_BEGIN(name) \
  const char* case_name = (name); \
  int case_ok = 1

#define CHECK(cond)                                    \
  do {                                                 \
    if (!(cond)) {                                     \
      case_ok = 0;                                     \
      printf("  check failed: %s (line %d)\n", #cond,  \
             __LINE__);                                \
    }                                                  \
  } while (0)

#define CASE_END()                                     \
  do {                                                 \
    printf("%s: %s\n", case_ok ? "PASS" : "FAIL",      \
           case_name);                                 \
    if (!case_ok) g_failures++;                        \
  } while (0)

/* Deterministic PRNG (splitmix-style LCG); fixed seed, so a failure
 * reproduces byte for byte on every host. */
static uint64_t g_rng = 0x57484631ull;
static uint32_t rnd32(void) {
  g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
  return (uint32_t)(g_rng >> 33);
}

static unsigned char pattern_byte(uint32_t slot, size_t i) {
  return (unsigned char)(slot * 131u + (uint32_t)i * 7u + (slot >> 3));
}

static void fill_pattern(unsigned char* p, size_t n, uint32_t slot) {
  size_t i;
  for (i = 0; i < n; i++) p[i] = pattern_byte(slot, i);
}

static int check_pattern(const unsigned char* p, size_t n, uint32_t slot) {
  size_t i;
  for (i = 0; i < n; i++)
    if (p[i] != pattern_byte(slot, i)) return 0;
  return 1;
}

/* Arenas. Aligned via a union with a maximally aligned member type
 * built from base types only (long double forces 16 on the hosts
 * this test runs on; the union member trick keeps the test itself
 * portable C99). */
typedef union {
  long double align_ld;
  void* align_ptr;
  unsigned char bytes[6 * 1024 * 1024];
} arena6m_t;
static arena6m_t g_arena;

/* Grow-callback fixtures: a reserve the callback hands out in
 * extents, and a refusal budget the exhaustion case drains. */
static arena6m_t g_reserve;
static size_t g_reserve_used = 0;
static size_t g_grow_budget = 0; /* bytes the callback may still grant */

/* Standalone arena for the bad-free regression case: fresh heaps
 * per scenario, so no scenario inherits another's block layout. */
typedef union {
  long double align_ld;
  void* align_ptr;
  unsigned char bytes[1024 * 1024];
} arena1m_t;
static arena1m_t g_bad;

/* Small standalone arena for the grow case's initial extent, so it
 * never overlaps the reserve the callback hands out. */
typedef union {
  long double align_ld;
  void* align_ptr;
  unsigned char bytes[128 * 1024];
} arena128k_t;
static arena128k_t g_small;

static void* test_grow(wh_heap_t* heap, size_t min_extra,
                       size_t* out_bytes, void* user) {
  size_t grant;
  (void)heap;
  (void)user;
  if (min_extra > g_grow_budget) return NULL;
  grant = min_extra;
  if (grant > g_grow_budget) grant = g_grow_budget;
  if (g_reserve_used + grant > sizeof(g_reserve.bytes)) return NULL;
  *out_bytes = grant;
  g_grow_budget -= grant;
  {
    void* p = g_reserve.bytes + g_reserve_used;
    g_reserve_used += grant;
    return p;
  }
}

/* Shadow model for the churn case. */
#define MODEL_SLOTS 512
typedef struct {
  unsigned char* ptr;
  size_t size;
} slot_t;
static slot_t g_model[MODEL_SLOTS];

/* Review M7 fixture: a lock that records whether it is held, and a
 * grow callback that fails the case if it runs with the lock held
 * and re-enters the heap with a small allocation (which a held,
 * non-recursive lock would deadlock on). */
static int g_lock_held, g_reent_bad, g_reent_allocs, g_in_grow;
static wh_heap_t* g_reent_heap;
static void flag_lock(void* u) {
  (void)u;
  if (g_lock_held) g_reent_bad = 1; /* re-entered while held */
  g_lock_held = 1;
}
static void flag_unlock(void* u) {
  (void)u;
  g_lock_held = 0;
}
static void* reentrant_grow(wh_heap_t* heap, size_t min_extra,
                            size_t* out_bytes, void* user) {
  void* extent;
  if (g_lock_held) g_reent_bad = 1;
  extent = test_grow(heap, min_extra, out_bytes, user);
  if (!g_in_grow) {
    void* p;
    g_in_grow = 1;
    p = wh_malloc(g_reent_heap, 16);
    if (p != NULL) {
      g_reent_allocs++;
      wh_free(g_reent_heap, p);
    }
    g_in_grow = 0;
  }
  return extent;
}

static int model_verify_all(wh_heap_t* h) {
  int i;
  for (i = 0; i < MODEL_SLOTS; i++) {
    if (g_model[i].ptr != NULL) {
      if (!check_pattern(g_model[i].ptr, g_model[i].size, (uint32_t)i))
        return 0;
      if (wh_malloc_usable_size(h, g_model[i].ptr) < g_model[i].size)
        return 0;
    }
  }
  return 1;
}

int main(void) {
  wh_heap_t* h;

  /* ---- case: lifecycle over a caller arena ----------------------- */
  {
    CASE_BEGIN("lifecycle: create over caller arena, destroy");
    h = wh_heap_create(g_arena.bytes, 1024 * 1024, NULL);
    CHECK(h != NULL);
    {
      wh_stats_t st;
      wh_heap_stats(h, &st);
      CHECK(st.region_bytes > 0);
      CHECK(st.free_bytes > 0);
      CHECK(st.used_bytes == 0);
      CHECK(st.largest_free_block > 512 * 1024);
    }
    wh_heap_destroy(h);
    CHECK(wh_malloc(h, 16) == NULL); /* dead heap fails soft */
    wh_free(h, (void*)g_arena.bytes); /* ignored, no crash */
    CASE_END();
  }

  h = wh_heap_create(g_arena.bytes, 2 * 1024 * 1024, NULL);
  if (h == NULL) {
    printf("FAIL: could not create the main test heap\n");
    return 1;
  }

  /* ---- case: basic alloc, alignment, usable size ----------------- */
  {
    CASE_BEGIN("basic: alloc alignment and usable size");
    void* a = wh_malloc(h, 1);
    void* b = wh_malloc(h, 13);
    void* c = wh_malloc(h, 1000);
    void* z = wh_malloc(h, 0);
    CHECK(a != NULL && b != NULL && c != NULL && z != NULL);
    CHECK(((uintptr_t)a % WH_ALIGNMENT) == 0);
    CHECK(((uintptr_t)b % WH_ALIGNMENT) == 0);
    CHECK(((uintptr_t)c % WH_ALIGNMENT) == 0);
    CHECK(wh_malloc_usable_size(h, a) >= 1);
    CHECK(wh_malloc_usable_size(h, b) >= 13);
    CHECK(wh_malloc_usable_size(h, c) >= 1000);
    CHECK(a != b && b != c && a != c);
    memset(a, 0xa5, wh_malloc_usable_size(h, a));
    memset(c, 0x5a, wh_malloc_usable_size(h, c));
    wh_free(h, a);
    wh_free(h, b);
    wh_free(h, c);
    wh_free(h, z);
    wh_free(h, NULL); /* no-op */
    CHECK(wh_malloc_usable_size(h, NULL) == 0);
    CASE_END();
  }

  /* ---- case: calloc ---------------------------------------------- */
  {
    CASE_BEGIN("calloc: zero fill and overflow refusal");
    unsigned char* p = (unsigned char*)wh_calloc(h, 64, 16);
    int i, zero = 1;
    CHECK(p != NULL);
    for (i = 0; i < 64 * 16; i++)
      if (p[i] != 0) zero = 0;
    CHECK(zero);
    wh_free(h, p);
    CHECK(wh_calloc(h, (size_t)-1 / 2, 4) == NULL); /* overflow */
    p = (unsigned char*)wh_calloc(h, 0, 16);
    CHECK(p != NULL);
    wh_free(h, p);
    CASE_END();
  }

  /* ---- case: aligned allocations --------------------------------- */
  {
    CASE_BEGIN("aligned_alloc: 32/64/256/4096 alignment, free, usable");
    static const size_t aligns[] = {32, 64, 256, 4096};
    int i;
    for (i = 0; i < 4; i++) {
      size_t al = aligns[i];
      unsigned char* p =
          (unsigned char*)wh_aligned_alloc(h, al, 300);
      CHECK(p != NULL);
      CHECK(((uintptr_t)p % al) == 0);
      CHECK(wh_malloc_usable_size(h, p) >= 300);
      fill_pattern(p, 300, 900u + (uint32_t)i);
      CHECK(check_pattern(p, 300, 900u + (uint32_t)i));
      wh_free(h, p);
    }
    CHECK(wh_aligned_alloc(h, 24, 100) == NULL); /* not a power of 2 */
    CHECK(wh_aligned_alloc(h, 2, 100) == NULL);  /* below void* */
    CASE_END();
  }

  /* ---- case: realloc semantics ------------------------------------ */
  {
    CASE_BEGIN("realloc: grow and shrink preserve contents");
    unsigned char* p = (unsigned char*)wh_malloc(h, 100);
    unsigned char* q;
    CHECK(p != NULL);
    fill_pattern(p, 100, 42);
    q = (unsigned char*)wh_realloc(h, p, 5000); /* grow (may move) */
    CHECK(q != NULL);
    CHECK(check_pattern(q, 100, 42));
    fill_pattern(q, 5000, 43);
    p = (unsigned char*)wh_realloc(h, q, 64); /* shrink in place */
    CHECK(p == q);
    CHECK(check_pattern(p, 64, 43));
    q = (unsigned char*)wh_realloc(h, NULL, 128); /* acts as malloc */
    CHECK(q != NULL);
    wh_free(h, q);
    CHECK(wh_realloc(h, p, 0) == NULL); /* frees */
    CASE_END();
  }

  /* ---- case: realloc growing into a free successor ---------------- */
  {
    CASE_BEGIN("realloc: grow into free successor stays in place");
    unsigned char* a = (unsigned char*)wh_malloc(h, 200);
    unsigned char* b = (unsigned char*)wh_malloc(h, 200);
    unsigned char* c = (unsigned char*)wh_malloc(h, 200);
    unsigned char* na;
    CHECK(a != NULL && b != NULL && c != NULL);
    fill_pattern(a, 200, 7);
    wh_free(h, b); /* a's successor is now free */
    na = (unsigned char*)wh_realloc(h, a, 350);
    CHECK(na == a);
    CHECK(check_pattern(na, 200, 7));
    wh_free(h, na);
    wh_free(h, c);
    CASE_END();
  }

  /* ---- case: overlap stomp --------------------------------------- */
  {
    CASE_BEGIN("overlap: 64 full-payload fills never stomp a neighbor");
    unsigned char* ptrs[64];
    int i, ok = 1;
    for (i = 0; i < 64; i++) {
      ptrs[i] = (unsigned char*)wh_malloc(h, 257);
      if (ptrs[i] == NULL) ok = 0;
      else memset(ptrs[i], i + 1, wh_malloc_usable_size(h, ptrs[i]));
    }
    CHECK(ok);
    for (i = 0; i < 64; i++) {
      size_t n = wh_malloc_usable_size(h, ptrs[i]);
      size_t j;
      for (j = 0; j < n; j++)
        if (ptrs[i][j] != (unsigned char)(i + 1)) ok = 0;
    }
    CHECK(ok);
    for (i = 0; i < 64; i++) wh_free(h, ptrs[i]);
    CASE_END();
  }

  /* ---- case: randomized churn against the shadow model ------------ */
  {
    CASE_BEGIN("churn: 20000 randomized ops vs shadow model");
    int step, ok = 1;
    memset(g_model, 0, sizeof(g_model));
    for (step = 0; step < 20000 && ok; step++) {
      uint32_t r = rnd32();
      int slot = (int)(r % MODEL_SLOTS);
      int op = (int)((r >> 8) % 3);
      if (g_model[slot].ptr == NULL) {
        size_t n = 1 + (rnd32() % 6144);
        unsigned char* p = (unsigned char*)wh_malloc(h, n);
        if (p == NULL) {
          /* Acceptable only when no free block could serve it. */
          wh_stats_t st;
          wh_heap_stats(h, &st);
          if (st.largest_free_block >= n) { ok = 0; break; }
          continue;
        }
        fill_pattern(p, n, (uint32_t)slot);
        g_model[slot].ptr = p;
        g_model[slot].size = n;
      } else if (op == 0) {
        if (!check_pattern(g_model[slot].ptr, g_model[slot].size,
                           (uint32_t)slot)) { ok = 0; break; }
        wh_free(h, g_model[slot].ptr);
        g_model[slot].ptr = NULL;
        g_model[slot].size = 0;
      } else {
        size_t n = 1 + (rnd32() % 8192);
        size_t keep = g_model[slot].size < n ? g_model[slot].size : n;
        unsigned char* p =
            (unsigned char*)wh_realloc(h, g_model[slot].ptr, n);
        if (p == NULL) {
          /* Contract: the old block survives a failed realloc. */
          if (!check_pattern(g_model[slot].ptr, g_model[slot].size,
                             (uint32_t)slot)) { ok = 0; break; }
          continue;
        }
        if (!check_pattern(p, keep, (uint32_t)slot)) { ok = 0; break; }
        fill_pattern(p, n, (uint32_t)slot);
        g_model[slot].ptr = p;
        g_model[slot].size = n;
      }
      if (step % 5000 == 4999 && !model_verify_all(h)) { ok = 0; break; }
    }
    CHECK(ok);
    CHECK(model_verify_all(h));
    {
      int i;
      for (i = 0; i < MODEL_SLOTS; i++) {
        wh_free(h, g_model[i].ptr);
        g_model[i].ptr = NULL;
      }
    }
    {
      wh_stats_t st;
      wh_heap_stats(h, &st);
      CHECK(st.used_bytes == 0);
      CHECK(st.live_blocks == 0);
      /* Everything coalesced back: one region, essentially whole. */
      CHECK(st.free_blocks == 1);
      CHECK(st.largest_free_block > 1024 * 1024);
    }
    CASE_END();
  }

  /* ---- case: fragmentation and coalescing -------------------------- */
  {
    CASE_BEGIN("fragmentation: interleaved frees coalesce for a large alloc");
    void* smalls[256];
    void* bigs[16];
    int i;
    for (i = 0; i < 256; i++) smalls[i] = wh_malloc(h, 96);
    for (i = 0; i < 16; i++) bigs[i] = wh_malloc(h, 8192);
    for (i = 0; i < 256; i++) CHECK(smalls[i] != NULL);
    for (i = 0; i < 16; i++) CHECK(bigs[i] != NULL);
    /* Punch holes everywhere: every small, every other big. */
    for (i = 0; i < 256; i++) wh_free(h, smalls[i]);
    for (i = 0; i < 16; i += 2) wh_free(h, bigs[i]);
    {
      void* large = wh_malloc(h, 1024 * 1024);
      CHECK(large != NULL);
      wh_free(h, large);
    }
    for (i = 1; i < 16; i += 2) wh_free(h, bigs[i]);
    CASE_END();
  }

  /* ---- case: bad frees are safe no-ops (audit A-1 regressions) ------
   * wh_decode once trusted a candidate header derived from the word
   * before the payload. That word is the block's size_flags for a
   * plain block, and freeing SETS its bit 0, the same bit the decode
   * read as the over-aligned tag: a freed or interior pointer
   * manufactured a phantom header and corrupted the heap, crashed,
   * or (via realloc) read out of a dead block. Each scenario below
   * is one of the auditor's probes, on a fresh heap, asserting the
   * bad call changes nothing and the heap keeps working. */
  {
    CASE_BEGIN("bad frees: double, interior, stale realloc are safe no-ops");
    wh_stats_t before, after;
    /* s3: plain double free, neighbors live so no coalescing. */
    {
      wh_heap_t* f = wh_heap_create(g_bad.bytes, sizeof(g_bad.bytes), NULL);
      unsigned char* a = (unsigned char*)wh_malloc(f, 96);
      unsigned char* b = (unsigned char*)wh_malloc(f, 96);
      unsigned char* c = (unsigned char*)wh_malloc(f, 96);
      CHECK(f != NULL && a != NULL && b != NULL && c != NULL);
      fill_pattern(a, 96, 0x5a);
      wh_free(f, b);
      wh_heap_stats(f, &before);
      wh_free(f, b); /* the double free: must be a no-op */
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == before.used_bytes);
      CHECK(after.live_blocks == before.live_blocks);
      CHECK(after.free_blocks == before.free_blocks);
      CHECK(after.free_bytes == before.free_bytes);
      CHECK(wh_malloc_usable_size(f, b) == 0);
      wh_free(f, a);
      wh_free(f, c);
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == 0 && after.live_blocks == 0);
      CHECK(after.free_blocks == 1); /* drained heap fully coalesced */
      wh_heap_destroy(f);
    }
    /* s4: double free of a pointer whose block already coalesced
     * backward into its freed predecessor. */
    {
      wh_heap_t* f = wh_heap_create(g_bad.bytes, sizeof(g_bad.bytes), NULL);
      unsigned char* a = (unsigned char*)wh_malloc(f, 64);
      unsigned char* b = (unsigned char*)wh_malloc(f, 64);
      void* p;
      CHECK(f != NULL && a != NULL && b != NULL);
      fill_pattern(a, 64, 0x41);
      fill_pattern(b, 64, 0x42);
      wh_free(f, a);
      wh_free(f, b); /* merges backward into a's block */
      wh_heap_stats(f, &before);
      CHECK(before.live_blocks == 0 && before.free_blocks == 1);
      wh_free(f, b); /* stale pointer into the merged block: no-op */
      wh_heap_stats(f, &after);
      CHECK(after.live_blocks == 0 && after.free_blocks == 1);
      CHECK(after.free_bytes == before.free_bytes);
      p = wh_malloc(f, 512); /* heap still serves */
      CHECK(p != NULL);
      wh_free(f, p);
      wh_heap_destroy(f);
    }
    /* s1: interior free of a zeroed live block. */
    {
      wh_heap_t* f = wh_heap_create(g_bad.bytes, sizeof(g_bad.bytes), NULL);
      unsigned char* p = (unsigned char*)wh_malloc(f, 256);
      unsigned char* q = (unsigned char*)wh_malloc(f, 64);
      CHECK(f != NULL && p != NULL && q != NULL);
      memset(p, 0, wh_malloc_usable_size(f, p));
      wh_heap_stats(f, &before);
      wh_free(f, p + 16); /* interior pointer: no-op */
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == before.used_bytes);
      CHECK(after.live_blocks == before.live_blocks);
      CHECK(after.free_blocks == before.free_blocks);
      CHECK(wh_malloc_usable_size(f, p) >= 256); /* real block lives */
      wh_free(f, p);
      wh_free(f, q);
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == 0 && after.free_blocks == 1);
      wh_heap_destroy(f);
    }
    /* s2: interior free of a nonzero-filled live block (the probe
     * that segfaulted in the free-list unlink). */
    {
      wh_heap_t* f = wh_heap_create(g_bad.bytes, sizeof(g_bad.bytes), NULL);
      unsigned char* p = (unsigned char*)wh_malloc(f, 256);
      size_t n, i;
      int intact = 1;
      CHECK(f != NULL && p != NULL);
      n = wh_malloc_usable_size(f, p);
      memset(p, 0x41, n);
      wh_heap_stats(f, &before);
      wh_free(f, p + 16); /* interior pointer: no-op, no crash */
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == before.used_bytes);
      CHECK(after.live_blocks == before.live_blocks);
      for (i = 0; i < n; i++)
        if (p[i] != 0x41) intact = 0;
      CHECK(intact); /* the live block was not carved up */
      wh_free(f, p);
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == 0 && after.free_blocks == 1);
      wh_heap_destroy(f);
    }
    /* s5: realloc of a freed plain pointer must not read the dead
     * block (the probe recovered its 0x5a fill in the new block). */
    {
      wh_heap_t* f = wh_heap_create(g_bad.bytes, sizeof(g_bad.bytes), NULL);
      unsigned char* a = (unsigned char*)wh_malloc(f, 64);
      unsigned char* p = (unsigned char*)wh_malloc(f, 64);
      unsigned char* c = (unsigned char*)wh_malloc(f, 64);
      CHECK(f != NULL && a != NULL && p != NULL && c != NULL);
      memset(p, 0x5a, 64);
      wh_free(f, p); /* bracketed by live blocks: no coalescing */
      wh_heap_stats(f, &before);
      CHECK(wh_realloc(f, p, 32) == NULL);
      CHECK(wh_malloc_usable_size(f, p) == 0);
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == before.used_bytes);
      CHECK(after.live_blocks == before.live_blocks);
      CHECK(after.free_blocks == before.free_blocks);
      wh_free(f, a);
      wh_free(f, c);
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == 0 && after.free_blocks == 1);
      wh_heap_destroy(f);
    }
    /* Over-aligned double free: the tag decodes to the real header,
     * so this exercises the is_free guard itself. */
    {
      wh_heap_t* f = wh_heap_create(g_bad.bytes, sizeof(g_bad.bytes), NULL);
      unsigned char* p = (unsigned char*)wh_aligned_alloc(f, 256, 300);
      CHECK(f != NULL && p != NULL);
      wh_free(f, p);
      wh_heap_stats(f, &before);
      wh_free(f, p); /* no-op via the guard */
      wh_heap_stats(f, &after);
      CHECK(after.used_bytes == before.used_bytes);
      CHECK(after.live_blocks == before.live_blocks);
      CHECK(after.free_blocks == before.free_blocks);
      wh_heap_destroy(f);
    }
    CASE_END();
  }

  /* ---- case: exhaustion without a grow callback --------------------- */
  {
    CASE_BEGIN("exhaustion: fixed arena fails soft with NULL");
    wh_heap_t* fixed =
        wh_heap_create(g_reserve.bytes, 128 * 1024, NULL);
    void* keep[64];
    int i, n = 0;
    CHECK(fixed != NULL);
    for (i = 0; i < 64; i++) {
      keep[i] = wh_malloc(fixed, 4096);
      if (keep[i] != NULL) n++;
    }
    CHECK(n > 0 && n < 64); /* filled up, then refused */
    CHECK(wh_malloc(fixed, 60 * 1024) == NULL);
    /* The heap still works for what fits. */
    {
      void* p = wh_malloc(fixed, 64);
      CHECK(p != NULL);
      wh_free(fixed, p);
    }
    for (i = 0; i < n; i++) wh_free(fixed, keep[i]);
    wh_heap_destroy(fixed);
    CASE_END();
  }

  /* ---- case: growth through the callback, then refusal -------------- */
  {
    CASE_BEGIN("grow: callback extents adopted; refusal fails soft");
    wh_options_t opt;
    wh_heap_t* growing;
    void* blocks[32];
    int i, n = 0;
    memset(&opt, 0, sizeof(opt));
    opt.grow = test_grow;
    g_reserve_used = 0;
    g_grow_budget = 1024 * 1024; /* 1 MiB of growth on offer */
    growing = wh_heap_create(g_small.bytes, sizeof(g_small.bytes), &opt);
    CHECK(growing != NULL);
    for (i = 0; i < 32; i++) {
      blocks[i] = wh_malloc(growing, 32 * 1024);
      if (blocks[i] != NULL) n++;
    }
    /* The 128 KiB arena alone holds about three 32 KiB blocks; the
     * rest (grants of about 49 KiB each against the 1 MiB budget)
     * must have come through the grow callback. */
    CHECK(n >= 20);
    /* Budget spent: the next large request must refuse, not abort. */
    CHECK(wh_malloc(growing, 512 * 1024) == NULL);
    {
      void* p = wh_malloc(growing, 128);
      CHECK(p != NULL);
      wh_free(growing, p);
    }
    {
      wh_stats_t st;
      wh_heap_stats(growing, &st);
      CHECK(st.region_bytes > 1024 * 1024); /* extents were adopted */
    }
    for (i = 0; i < n; i++) wh_free(growing, blocks[i]);
    wh_heap_destroy(growing);
    CASE_END();
  }

  /* ---- case: grow callback may re-enter the heap (review M7) -------- */
  {
    CASE_BEGIN("grow: callback runs unlocked and may allocate");
    wh_options_t opt;
    wh_heap_t* growing;
    void* blocks[64];
    int i, n = 0;
    memset(&opt, 0, sizeof opt);
    opt.grow = reentrant_grow;
    opt.lock = flag_lock;
    opt.unlock = flag_unlock;
    g_reserve_used = 0;
    g_grow_budget = 1024 * 1024;
    g_reent_bad = 0;
    g_reent_allocs = 0;
    growing = wh_heap_create(g_small.bytes, sizeof(g_small.bytes), &opt);
    CHECK(growing != NULL);
    g_reent_heap = growing;
    for (i = 0; i < 64; i++) {
      blocks[i] = wh_malloc(growing, 32 * 1024);
      if (blocks[i] == NULL) break;
      n++;
    }
    CHECK(n > 8);
    CHECK(g_reent_allocs > 0);   /* the callback did allocate */
    CHECK(g_reent_bad == 0);     /* and never ran with the lock held */
    for (i = 0; i < n; i++) wh_free(growing, blocks[i]);
    wh_heap_destroy(growing);
    CASE_END();
  }

  /* ---- case: default heap layer -------------------------------------- */
  {
    CASE_BEGIN("default heap: wh_libc_* convenience layer");
    wh_set_default_heap(h);
    CHECK(wh_default_heap() == h);
    {
      unsigned char* p = (unsigned char*)wh_libc_malloc(500);
      unsigned char* q = (unsigned char*)wh_libc_calloc(10, 10);
      int i, zero = 1;
      CHECK(p != NULL && q != NULL);
      for (i = 0; i < 100; i++)
        if (q[i] != 0) zero = 0;
      CHECK(zero);
      p = (unsigned char*)wh_libc_realloc(p, 5000);
      CHECK(p != NULL);
      wh_libc_free(p);
      wh_libc_free(q);
    }
    wh_set_default_heap(NULL);
    CHECK(wh_libc_malloc(16) == NULL);
    CASE_END();
  }

  wh_heap_destroy(h);

  if (g_failures == 0) {
    printf("WAFERHEAP TEST: PASS\n");
    return 0;
  }
  printf("WAFERHEAP TEST: FAIL (%d case(s))\n", g_failures);
  return 1;
}
