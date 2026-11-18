/* lithoheap.h ... LithoRoot's shared, lock-free heap.
 *
 * Replaces the old pthread-mutex hooks around one WaferHeap. Design
 * (JD's doctrine: no locks or spin gates of any kind):
 *
 *  - Every thread allocates from its own arena: one WaferHeap that
 *    only the thread that owns the arena ever touches, so WaferHeap
 *    runs with no lock hooks at all.
 *  - Arenas live in a fixed table (LH_MAX_ARENAS). A thread takes a
 *    free arena with one compare-and-swap on its state word and
 *    gives it back at thread exit with one store. A returned arena
 *    keeps its memory and its outstanding blocks; the next thread to
 *    claim it inherits both. Arenas are never destroyed, so no
 *    reader can ever see one freed.
 *  - Memory comes from one region in chunks of LH_CHUNK bytes. The
 *    region bump pointer advances by compare-and-swap; a chunk
 *    table records which arena owns each chunk, so free() finds a
 *    block's owner with one atomic load.
 *  - A free from a thread that does not own the block pushes the
 *    block onto the owning arena's remote list: an intrusive
 *    multi-producer single-consumer stack (CAS push). The owner
 *    takes the whole list with one atomic exchange and frees the
 *    blocks into its WaferHeap at its next malloc or free. Taking
 *    the whole list (never popping one node) is what keeps the
 *    stack free of ABA.
 *
 * Why this design over size-class caches: WaferHeap already gives
 * O(1) good-fit allocation and coalescing; per-thread arenas reuse
 * it unchanged and add only the ownership protocol. Size-class
 * caches in front of one shared heap would still need the shared
 * heap to be lock-free underneath, which is the problem we started
 * with.
 *
 * Progress: malloc and free are lock-free (a CAS loop only retries
 * when another thread made progress). The only waits are the
 * host's memory.grow, which is itself atomic.
 */
#ifndef LITHOHEAP_H
#define LITHOHEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LH_MAX_ARENAS 256u
#define LH_CHUNK_SHIFT 20u
#define LH_CHUNK ((size_t)1 << LH_CHUNK_SHIFT) /* 1 MiB */

/* Commit callback: make [base, end) usable. On wasm this grows
 * linear memory until memory.size covers end; native tests commit a
 * pre-reserved block. Returns 1 on success, 0 when out of memory.
 * May be called concurrently and must tolerate that. */
typedef int (*lh_commit_fn)(uintptr_t end, void* user);

/* Install the shared heap over [base, limit). Called once, before
 * any second thread exists (crt1 on wasm). Returns 1 on success. */
int lh_install(uintptr_t base, uintptr_t limit, lh_commit_fn commit,
               void* user);
int lh_installed(void);

void* lh_malloc(size_t size);
void lh_free(void* ptr);
void* lh_realloc(void* ptr, size_t size);
void* lh_calloc(size_t count, size_t size);
void* lh_aligned_alloc(size_t alignment, size_t size);
size_t lh_usable_size(const void* ptr);

/* Give the calling thread's arena back (thread exit). Remote frees
 * that arrive later wait on the arena's list for the next owner. */
void lh_thread_release(void);

/* Test hooks: the calling thread's arena index, or -1; and the
 * number of blocks waiting on remote lists across all arenas. */
int lh_debug_arena_index(void);
size_t lh_debug_remote_pending(void);

#ifdef __cplusplus
}
#endif
#endif
