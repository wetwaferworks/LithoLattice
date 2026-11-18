/* lithothread_heap.c ... the lock-free shared heap (see lithoheap.h).
 *
 * The pthread mutex that used to wrap the one WaferHeap is gone.
 * Each thread owns an arena (a WaferHeap with no lock hooks); a
 * cross-thread free goes onto the owner's MPSC remote list. */
#include <string.h>

#include "lithoheap.h"
#include "waferheap.h"

#if defined(_MSC_VER)
#define LH_TLS __declspec(thread)
#else
#define LH_TLS _Thread_local
#endif

#define LH_ACQ __ATOMIC_ACQUIRE
#define LH_REL __ATOMIC_RELEASE
#define LH_RLX __ATOMIC_RELAXED

#define LH_MAX_CHUNKS 4096u /* 4 GiB of 1 MiB chunks: all of wasm32 */

typedef struct lh_arena {
  uint32_t owned;       /* 0 free, 1 owned; CAS to claim */
  wh_heap_t* heap;      /* owner-only after creation */
  void* remote;         /* MPSC stack head of foreign frees */
} lh_arena_t;

static lh_arena_t lh__arenas[LH_MAX_ARENAS];
static uint16_t lh__chunk_owner[LH_MAX_CHUNKS]; /* arena index + 1 */
static uintptr_t lh__base, lh__limit, lh__brk;
static lh_commit_fn lh__commit;
static void* lh__commit_user;
static int lh__ready;
static LH_TLS int lh__mine = -1;

int lh_install(uintptr_t base, uintptr_t limit, lh_commit_fn commit,
               void* user) {
  if (commit == NULL || limit <= base) return 0;
  base = (base + LH_CHUNK - 1u) & ~(uintptr_t)(LH_CHUNK - 1u);
  if (limit <= base) return 0;
  if ((limit - base) >> LH_CHUNK_SHIFT > LH_MAX_CHUNKS)
    limit = base + ((uintptr_t)LH_MAX_CHUNKS << LH_CHUNK_SHIFT);
  lh__base = base;
  lh__limit = limit;
  lh__commit = commit;
  lh__commit_user = user;
  __atomic_store_n(&lh__brk, base, LH_RLX);
  __atomic_store_n(&lh__ready, 1, LH_REL);
  return 1;
}

int lh_installed(void) { return __atomic_load_n(&lh__ready, LH_ACQ); }

/* Take n chunks from the region for arena index idx. Lock-free: the
 * bump pointer moves by CAS after the commit covers the new end, so
 * a failed commit never strands address space. */
static void* lh__take_chunks(size_t n, unsigned idx) {
  uintptr_t cur = __atomic_load_n(&lh__brk, LH_RLX), end;
  size_t i, first;
  for (;;) {
    if (n > (lh__limit - cur) >> LH_CHUNK_SHIFT) return NULL;
    end = cur + (n << LH_CHUNK_SHIFT);
    if (!lh__commit(end, lh__commit_user)) return NULL;
    if (__atomic_compare_exchange_n(&lh__brk, &cur, end, 0, LH_ACQ, LH_RLX))
      break;
  }
  first = (cur - lh__base) >> LH_CHUNK_SHIFT;
  for (i = 0; i < n; ++i)
    __atomic_store_n(&lh__chunk_owner[first + i], (uint16_t)(idx + 1u), LH_REL);
  return (void*)cur;
}

static void* lh__grow(wh_heap_t* heap, size_t min_extra, size_t* out_bytes,
                      void* user) {
  unsigned idx = (unsigned)(uintptr_t)user;
  size_t n;
  void* p;
  (void)heap;
  if (min_extra > ((size_t)-1) - 2u * LH_CHUNK) return NULL;
  n = (min_extra + 4096u + LH_CHUNK - 1u) >> LH_CHUNK_SHIFT;
  if (n == 0) n = 1;
  p = lh__take_chunks(n, idx);
  if (p == NULL) return NULL;
  *out_bytes = n << LH_CHUNK_SHIFT;
  return p;
}

static int lh__owner_of(const void* ptr) {
  uintptr_t p = (uintptr_t)ptr;
  uint16_t o;
  if (p < lh__base || p >= lh__limit) return -1;
  o = __atomic_load_n(&lh__chunk_owner[(p - lh__base) >> LH_CHUNK_SHIFT],
                      LH_ACQ);
  return (int)o - 1;
}

static void lh__drain(lh_arena_t* a) {
  void* n;
  if (__atomic_load_n(&a->remote, LH_RLX) == NULL) return;
  n = __atomic_exchange_n(&a->remote, NULL, LH_ACQ);
  while (n != NULL) {
    void* next = *(void**)n;
    wh_free(a->heap, n);
    n = next;
  }
}

/* The calling thread's arena, claiming one on first use. */
static lh_arena_t* lh__arena(void) {
  unsigned i;
  if (lh__mine >= 0) {
    lh__drain(&lh__arenas[lh__mine]);
    return &lh__arenas[lh__mine];
  }
  if (!lh_installed()) return NULL;
  for (i = 0; i < LH_MAX_ARENAS; ++i) {
    lh_arena_t* a = &lh__arenas[i];
    uint32_t expect = 0;
    if (__atomic_load_n(&a->owned, LH_RLX) != 0u) continue;
    if (!__atomic_compare_exchange_n(&a->owned, &expect, 1u, 0, LH_ACQ, LH_RLX))
      continue;
    if (a->heap == NULL) {
      wh_options_t opts;
      void* chunk = lh__take_chunks(1, i);
      memset(&opts, 0, sizeof opts);
      opts.grow = lh__grow;
      opts.grow_user = (void*)(uintptr_t)i;
      if (chunk == NULL ||
          (a->heap = wh_heap_create(chunk, LH_CHUNK, &opts)) == NULL) {
        __atomic_store_n(&a->owned, 0u, LH_REL);
        return NULL;
      }
    }
    lh__mine = (int)i;
    lh__drain(a);
    return a;
  }
  return NULL; /* every arena owned: more live threads than arenas */
}

void lh_thread_release(void) {
  int i = lh__mine;
  if (i < 0) return;
  lh__drain(&lh__arenas[i]);
  lh__mine = -1;
  __atomic_store_n(&lh__arenas[i].owned, 0u, LH_REL);
}

void* lh_malloc(size_t size) {
  lh_arena_t* a = lh__arena();
  return a != NULL ? wh_malloc(a->heap, size) : NULL;
}

void* lh_aligned_alloc(size_t alignment, size_t size) {
  lh_arena_t* a = lh__arena();
  return a != NULL ? wh_aligned_alloc(a->heap, alignment, size) : NULL;
}

void* lh_calloc(size_t count, size_t size) {
  lh_arena_t* a = lh__arena();
  return a != NULL ? wh_calloc(a->heap, count, size) : NULL;
}

void lh_free(void* ptr) {
  int o;
  if (ptr == NULL) return;
  o = lh__owner_of(ptr);
  if (o < 0) return; /* not ours: WaferHeap's rule, a safe no-op */
  if (o == lh__mine) {
    wh_free(lh__arenas[o].heap, ptr);
    lh__drain(&lh__arenas[o]);
    return;
  }
  {
    lh_arena_t* a = &lh__arenas[o];
    void* head = __atomic_load_n(&a->remote, LH_RLX);
    do {
      *(void**)ptr = head;
    } while (!__atomic_compare_exchange_n(&a->remote, &head, ptr, 1, LH_REL,
                                          LH_RLX));
  }
}

size_t lh_usable_size(const void* ptr) {
  int o = ptr != NULL ? lh__owner_of(ptr) : -1;
  if (o < 0) return 0;
  /* The owner may be retagging this block's neighbour bit right now;
   * wh_usable_size_shared reads the header atomically. */
  return wh_usable_size_shared(ptr);
}

void* lh_realloc(void* ptr, size_t size) {
  int o;
  void* np;
  size_t old;
  if (ptr == NULL) return lh_malloc(size);
  if (size == 0) {
    lh_free(ptr);
    return NULL;
  }
  o = lh__owner_of(ptr);
  if (o < 0) return NULL;
  if (o == lh__mine) return wh_realloc(lh__arenas[o].heap, ptr, size);
  /* A foreign block: move it into our arena, hand the old one back. */
  old = wh_usable_size_shared(ptr);
  np = lh_malloc(size);
  if (np == NULL) return NULL;
  memcpy(np, ptr, old < size ? old : size);
  lh_free(ptr);
  return np;
}

int lh_debug_arena_index(void) { return lh__mine; }

size_t lh_debug_remote_pending(void) {
  size_t n = 0;
  unsigned i;
  for (i = 0; i < LH_MAX_ARENAS; ++i) {
    if (__atomic_load_n(&lh__arenas[i].owned, LH_ACQ) != 0u) continue;
    {
      void* p = __atomic_load_n(&lh__arenas[i].remote, LH_ACQ);
      while (p != NULL) {
        ++n;
        p = *(void**)p;
      }
    }
  }
  return n;
}
