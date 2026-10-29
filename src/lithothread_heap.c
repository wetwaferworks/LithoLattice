/* lithothread_heap.c ... WaferHeap's lock hooks, wired to LithoThread.
 *
 * WaferHeap's locking is injected (wh_options_t in waferheap.h):
 * every public heap entry point brackets its work with the lock
 * and unlock hooks, so a heap becomes thread-safe the moment a
 * real mutex backs those hooks. This file is that backing for
 * LithoThread: one process-wide LithoThread mutex in the normal
 * flavor, statically initialized, plus the options constructor
 * startup uses when it builds the default heap:
 *
 *   wh_options_t opts = lithothread_heap_options();
 *   opts.grow = my_grow_callback;   (the caller's growth story)
 *   heap = wh_heap_create(arena, bytes, &opts);
 *
 * Why one global mutex rather than one per heap: WaferHeap calls
 * the hooks with only the lock_user pointer to distinguish
 * heaps, and a per-heap mutex would have to live somewhere the
 * hooks can reach before the heap context exists... the options
 * are consumed at heap creation, when no per-heap state does.
 * One mutex for all heaps costs contention only between heaps,
 * which LithoRoot does not have (there is one default heap; a
 * second heap is a test or tool rarity), and buys a glue file
 * with no allocation, no initialization order, and no failure
 * mode: the mutex is a valid statically initialized object from
 * the first instruction. Link-order story, stated once: any link
 * that wants a thread-safe default heap includes this file and
 * builds its heap with these options; links that omit it keep
 * WaferHeap's NULL hooks and its single-threaded contract. No
 * symbol is overridden and no other file changes behavior.
 *
 * The mutex is the NORMAL flavor on purpose: its lock path is
 * pure atomics (no owner tracking, no errno, no allocation), so
 * the heap can take it inside any context the allocator itself
 * runs in, including inside the weave's spawn path, without a
 * recursion route back into thread machinery.
 */

#include "pthread.h"

#include "lithothread_internal.h"
#include "waferheap.h"

static pthread_mutex_t lt__heap_mu = PTHREAD_MUTEX_INITIALIZER;

static void lt__heap_lock(void* user) {
  (void)user;
  (void)pthread_mutex_lock(&lt__heap_mu);
}

static void lt__heap_unlock(void* user) {
  (void)user;
  (void)pthread_mutex_unlock(&lt__heap_mu);
}

wh_options_t lithothread_heap_options(void) {
  wh_options_t opts;
  opts.grow = NULL;
  opts.grow_user = NULL;
  opts.lock = lt__heap_lock;
  opts.unlock = lt__heap_unlock;
  opts.lock_user = NULL;
  return opts;
}
