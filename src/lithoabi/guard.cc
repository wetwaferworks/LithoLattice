/* guard.cc ... __cxa_guard_acquire / release / abort: thread-safe
 * initialization of function-local statics, over LithoThread.
 *
 * Guard word layout (the compiler reserves 8 bytes per guard in
 * static storage and its fast path reads the FIRST byte; on the
 * little-endian targets LithoRoot serves, that is the low byte):
 *
 *   low byte: 0 = uninitialized, 1 = initialized, 2 = initializing
 *   bits 32+: the owning thread's LithoWeave tid while initializing
 *
 * Protocol: the fast path is one atomic load; a thread that sees
 * "initializing" waits on a single global condition variable (the
 * classic one-lock design: guard waits are rare and short, and one
 * lock keeps the state machine auditable). The owner records its
 * tid so that recursive initialization (the initializer, directly
 * or transitively, touching its own static) is detected and ends in
 * terminate: the language leaves that case undefined, and a loud
 * end beats the silent deadlock the naive design produces.
 *
 * Memory ordering: all state-word traffic goes through the seq_cst
 * shim. The initializer's writes happen between the owner's
 * acquire (which set state 2) and its release store of state 1, and
 * a reader that observes state 1 through the atomic load has the
 * release edge it needs. Waiters re-read the word under the mutex
 * after every wakeup, so a broadcast can never be missed: release
 * and abort both change the word under the same mutex before
 * broadcasting.
 */

#include "pthread.h"    /* LithoThread */
#include "lithoweave.h" /* lw_current_tid */

#include "lithoabi_internal.h"

namespace __cxxabiv1 {
namespace {

pthread_mutex_t guard_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t guard_cond = PTHREAD_COND_INITIALIZER;

constexpr uint64_t GUARD_UNINITIALIZED = 0;
constexpr uint64_t GUARD_INITIALIZED = 1;
constexpr uint64_t GUARD_INITIALIZING = 2;

uint64_t guard_load(long long* guard) {
  return la_atomic_load64(reinterpret_cast<const uint64_t*>(guard));
}
void guard_store(long long* guard, uint64_t value) {
  la_atomic_store64(reinterpret_cast<uint64_t*>(guard), value);
}
uint64_t guard_pack(uint64_t state, int32_t tid) {
  return state | (static_cast<uint64_t>(static_cast<uint32_t>(tid)) << 32);
}

} /* namespace */

int __cxa_guard_acquire(long long* guard) {
  if (guard_load(guard) == GUARD_INITIALIZED) return 0;
  const int32_t self = lw_current_tid();
  (void)pthread_mutex_lock(&guard_mutex);
  for (;;) {
    const uint64_t word = guard_load(guard);
    const uint64_t state = word & 0xffu;
    if (state == GUARD_INITIALIZED) {
      (void)pthread_mutex_unlock(&guard_mutex);
      return 0;
    }
    if (state == GUARD_INITIALIZING) {
      const int32_t owner = static_cast<int32_t>(
          static_cast<uint32_t>(word >> 32));
      if (owner == self) {
        (void)pthread_mutex_unlock(&guard_mutex);
        lithoabi_terminate_now(
            "lithoabi: recursive initialization of a function-local "
            "static\n");
      }
      (void)pthread_cond_wait(&guard_cond, &guard_mutex);
      continue;
    }
    guard_store(guard, guard_pack(GUARD_INITIALIZING, self));
    (void)pthread_mutex_unlock(&guard_mutex);
    return 1;
  }
}

void __cxa_guard_release(long long* guard) {
  (void)pthread_mutex_lock(&guard_mutex);
  guard_store(guard, GUARD_INITIALIZED);
  (void)pthread_cond_broadcast(&guard_cond);
  (void)pthread_mutex_unlock(&guard_mutex);
}

void __cxa_guard_abort(long long* guard) {
  const int32_t self = lw_current_tid();
  (void)pthread_mutex_lock(&guard_mutex);
  const uint64_t word = guard_load(guard);
  if ((word & 0xffu) == GUARD_INITIALIZING &&
      static_cast<int32_t>(static_cast<uint32_t>(word >> 32)) == self) {
    guard_store(guard, GUARD_UNINITIALIZED);
    (void)pthread_cond_broadcast(&guard_cond);
  }
  /* Aborting a guard this thread does not own is a caller bug the
   * ABI leaves undefined; the defensive choice is to leave the
   * state untouched rather than corrupt another thread's init. */
  (void)pthread_mutex_unlock(&guard_mutex);
}

} /* namespace __cxxabiv1 */
