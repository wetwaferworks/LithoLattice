/* waferheap.h ... WaferHeap, the LithoRoot allocator.
 *
 * WaferHeap is a TLSF allocator (two-level segregated fit, after the
 * design of Masmano et al.) written fresh for LithoRoot: bounded
 * O(1) malloc and free, immediate coalescing of free blocks through
 * boundary tags, and a segregated search that never walks a list
 * whose length depends on the heap's history. It replaces the vendor
 * runtime's dlmalloc once LithoLibC wraps it.
 *
 * Contract highlights (docs/WAFERHEAP.md has the full note):
 *
 *  - Explicit context. Every heap is a wh_heap_t created over a
 *    caller-supplied arena. The API keeps no hidden state; a process
 *    wide default heap exists only as a convenience layer for
 *    LithoLibC (the wh_libc_* functions at the bottom).
 *
 *  - Growth is injected. When an allocation cannot be served, the
 *    heap calls the grow callback from wh_options_t once; the
 *    callback returns a fresh extent of memory which the heap adopts
 *    as an additional region (aligning the base up internally, so a
 *    callback slicing a larger buffer need not track our alignment),
 *    or NULL to refuse, in which case the allocation fails with
 *    NULL. WaferHeap never aborts, traps or
 *    calls exit on failure. On wasm the callback is where
 *    lithosys_memory_grow gets wired (see lithosys.h: memory may
 *    only grow through the host, so the host enforces its cap and
 *    WaferHeap sees a refusal, never a trap). On native hosts the
 *    same heap code runs over malloc or mmap backed extents.
 *
 *  - Locking is injected. wh_options_t carries lock and unlock hooks
 *    (NULL means no locking, the single-threaded default). Every
 *    public entry point takes the lock, so LithoThread can later
 *    make a shared heap thread-safe without an API change.
 *
 *  - Determinism note. For a fixed sequence of calls the allocator
 *    is fully deterministic: same history, same addresses, on every
 *    platform with the same pointer width. Even so, addresses are a
 *    property of allocation history, which is not simulation state.
 *    Simulation code must never let an address, a pointer ordering
 *    or an allocation-order iteration feed sim-visible behavior.
 *    That is a rule for callers, stated here once; the allocator
 *    does not spend cycles enforcing it.
 *
 * This header is strict portable C99: no platform headers, no
 * vendor headers, fixed include set, extern "C" guarded.
 */
#ifndef WAFERHEAP_H
#define WAFERHEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAFERHEAP_VERSION 1

/* Alignment guaranteed for every pointer the plain allocation family
 * returns: two words (8 bytes on 32-bit targets, 16 on 64-bit), so
 * any C99 type including double and long long is served, and the
 * 16-byte alignment SIMD payloads need holds on 64-bit hosts.
 * wh_aligned_alloc serves larger alignments. */
#define WH_ALIGNMENT (2u * sizeof(void*))

typedef struct wh_heap wh_heap_t;

/* Grow callback. Called with the heap lock held when an allocation
 * needs min_extra_bytes more payload room than the heap can serve.
 * Returns the base of a new extent and stores its size in
 * out_region_bytes. The extent should offer at least
 * min_extra_bytes plus a small overhead; a shorter grant that
 * cannot serve the pending request is adopted anyway (it joins the
 * heap) but the pending allocation still fails. The base is aligned
 * up internally; alignment the callback provides is not required.
 * Returns NULL to refuse;
 * the pending allocation then fails with NULL. The extent is adopted
 * permanently: WaferHeap never hands memory back, matching wasm
 * linear memory, which cannot shrink. */
typedef void* (*wh_grow_fn)(wh_heap_t* heap, size_t min_extra_bytes,
                            size_t* out_region_bytes, void* user);

/* Lock hooks. Either may be NULL (no locking). lock and unlock are
 * called in balanced pairs around every public operation, with the
 * user pointer passed through. The grow callback above runs with
 * the lock RELEASED (review M7), so a grow implementation may
 * re-enter the heap (allocate, log through malloc); it may run
 * concurrently with other operations on the heap, and with another
 * thread's grow. */
typedef void (*wh_lock_fn)(void* user);

typedef struct wh_options {
  wh_grow_fn grow;      /* NULL: fixed arena, never grows */
  void* grow_user;
  wh_lock_fn lock;      /* NULL: no locking */
  wh_lock_fn unlock;    /* NULL: no locking */
  void* lock_user;
} wh_options_t;

/* Bytes of caller memory wh_heap_init needs for the heap context.
 * Constant for a given build of WaferHeap. */
size_t wh_heap_context_size(void);

/* Place a heap context in context_mem (wh_heap_context_size bytes,
 * WH_ALIGNMENT aligned) and build the heap over the arena extent
 * [arena, arena + arena_bytes). options may be NULL for all
 * defaults. Returns the context as a wh_heap_t, or NULL when the
 * arena is too small to hold even one minimal block. The arena must
 * outlive the heap. */
wh_heap_t* wh_heap_init(void* context_mem, void* arena, size_t arena_bytes,
                        const wh_options_t* options);

/* Convenience form: the context is carved from the front of the
 * arena itself, so the caller supplies one extent and nothing else.
 * arena_bytes must exceed wh_heap_context_size plus one minimal
 * block. Returns NULL when the arena is too small. */
wh_heap_t* wh_heap_create(void* arena, size_t arena_bytes,
                          const wh_options_t* options);

/* Retire a heap. Outstanding allocations are simply abandoned (the
 * arena belongs to the caller). The context is invalidated so stale
 * handles fail soft: later calls on it return NULL or do nothing.
 * NULL is accepted and does nothing. */
void wh_heap_destroy(wh_heap_t* heap);

/* Adopt an additional extent as a new region of the heap. The grow
 * callback path uses this internally; hosts may also call it
 * directly to pre-extend a heap. Returns 1 on success, 0 when the
 * extent is unusable (too small, misaligned, or the heap is dead). */
int wh_heap_add_region(wh_heap_t* heap, void* region, size_t region_bytes);

/* ---- allocation family ---------------------------------------------
 * All of these take the heap lock. Failure returns NULL, always,
 * with the heap left consistent and usable. */

/* Allocate at least size bytes, WH_ALIGNMENT aligned. Size 0
 * returns a unique freeable pointer to a minimal block. */
void* wh_malloc(wh_heap_t* heap, size_t size);

/* Release a pointer from this heap. NULL is a no-op. A pointer that
 * does not belong to this heap is ignored rather than corrupting
 * anything (it is a caller bug; the heap declines to make it worse).
 * Releasing the same pointer twice is a caller bug with the usual
 * undefined consequences; nothing here can detect it cheaply. */
void wh_free(wh_heap_t* heap, void* ptr);

/* Resize an allocation, preserving the first min(old, new) payload
 * bytes. NULL ptr behaves as wh_malloc; size 0 frees and returns
 * NULL. Shrinking and growing in place are done without copying when
 * the block geometry allows. A pointer obtained from wh_aligned_alloc
 * with an alignment above WH_ALIGNMENT keeps its contents but the
 * result is only WH_ALIGNMENT aligned (the realloc contract has no
 * alignment parameter; LithoLibC documents the same caveat). */
void* wh_realloc(wh_heap_t* heap, void* ptr, size_t size);

/* Allocate count * size bytes, zero filled. Returns NULL when the
 * product overflows size_t or the heap cannot serve it. */
void* wh_calloc(wh_heap_t* heap, size_t count, size_t size);

/* Allocate size bytes aligned to alignment, which must be a power
 * of two at least sizeof(void*). The returned pointer may be passed
 * to wh_free, wh_realloc and wh_malloc_usable_size like any other.
 * Over-aligned blocks carry a small extra overhead (a back pointer
 * word inside the block). */
void* wh_aligned_alloc(wh_heap_t* heap, size_t alignment, size_t size);

/* Payload bytes usable at ptr (the full block payload for plain
 * allocations; bytes from ptr to the block end for over-aligned
 * ones). Returns 0 for NULL or a pointer foreign to the heap. */
size_t wh_malloc_usable_size(wh_heap_t* heap, const void* ptr);

/* Usable size of a LIVE block of any heap, from any thread, without
 * the heap context (atomic header reads, no validation). */
size_t wh_usable_size_shared(const void* ptr);

/* ---- introspection ---------------------------------------------------
 * The only statistics WaferHeap keeps: counters maintained
 * incrementally on the allocation paths, plus largest_free_block,
 * which is computed by scanning the free lists when asked (rare,
 * diagnostic use). Byte counts are payload capacities (block size
 * minus the two word block header), not requested sizes. */
typedef struct wh_stats {
  size_t region_bytes;        /* total extent bytes adopted */
  size_t used_bytes;          /* payload capacity of live blocks */
  size_t free_bytes;          /* payload capacity of free blocks */
  size_t peak_used_bytes;     /* high water mark of used_bytes */
  size_t largest_free_block;  /* payload capacity, computed on call */
  uint32_t live_blocks;
  uint32_t free_blocks;
} wh_stats_t;

void wh_heap_stats(wh_heap_t* heap, wh_stats_t* out_stats);

/* ---- default heap: the convenience layer for LithoLibC ---------------
 * One process-wide default heap pointer, set once at startup before
 * threads run (the set and get are deliberately unsynchronized; the
 * wh_libc_* calls themselves take the heap's lock like any other
 * call). With no default heap set, allocations return NULL and frees
 * do nothing. LithoLibC maps its malloc family onto these. */
void wh_set_default_heap(wh_heap_t* heap);
wh_heap_t* wh_default_heap(void);

void* wh_libc_malloc(size_t size);
void wh_libc_free(void* ptr);
void* wh_libc_realloc(void* ptr, size_t size);
void* wh_libc_calloc(size_t count, size_t size);
void* wh_libc_aligned_alloc(size_t alignment, size_t size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAFERHEAP_H */
