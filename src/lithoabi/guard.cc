/* guard.cc ... __cxa_guard_acquire / release / abort: thread-safe
 * initialization of function-local statics, over LithoThread.
 *
 * Guard word layout. The width is the target's (cxxabi.h,
 * __lithoabi_guard_t): 32 bits on wasm32 (ARM C++ ABI guard; the
 * compiler emits `i32, align 4` and its fast path tests bit 0 of
 * the first byte), 64 bits on the Itanium native targets (the fast
 * path reads the first byte). Every access below is exactly that
 * width: a 64-bit access to a 4-byte wasm guard would overrun into
 * the neighbouring static and trap as a misaligned atomic.
 *
 *   low byte: 0 = uninitialized, 1 = initialized, 2 = initializing
 *   upper bits: the owning thread's LithoWeave tid while
 *     initializing (bits 32..63 on 64-bit guards; the low 24 bits
 *     of the tid in bits 8..31 on 32-bit guards, which only
 *     weakens recursion detection for tids 2^24 apart)
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

typedef __lithoabi_guard_t guard_t;

constexpr uint64_t GUARD_UNINITIALIZED = 0;
constexpr uint64_t GUARD_INITIALIZED = 1;
constexpr uint64_t GUARD_INITIALIZING = 2;

#if defined(__wasm__) || defined(__arm__)
static_assert(sizeof(guard_t) == 4, "ARM/wasm C++ ABI guard is 32 bits");
constexpr unsigned GUARD_OWNER_SHIFT = 8;
constexpr uint32_t GUARD_OWNER_MASK = 0xffffffu;
uint64_t guard_load(guard_t* guard) {
  return la_atomic_load32(reinterpret_cast<const uint32_t*>(guard));
}
void guard_store(guard_t* guard, uint64_t value) {
  la_atomic_store32(reinterpret_cast<uint32_t*>(guard),
                    static_cast<uint32_t>(value));
}
#else
static_assert(sizeof(guard_t) == 8, "Itanium C++ ABI guard is 64 bits");
constexpr unsigned GUARD_OWNER_SHIFT = 32;
constexpr uint32_t GUARD_OWNER_MASK = 0xffffffffu;
uint64_t guard_load(guard_t* guard) {
  return la_atomic_load64(reinterpret_cast<const uint64_t*>(guard));
}
void guard_store(guard_t* guard, uint64_t value) {
  la_atomic_store64(reinterpret_cast<uint64_t*>(guard), value);
}
#endif
uint32_t guard_owner_bits(int32_t tid) {
  return static_cast<uint32_t>(tid) & GUARD_OWNER_MASK;
}
uint64_t guard_pack(uint64_t state, int32_t tid) {
  return state | (static_cast<uint64_t>(guard_owner_bits(tid))
                  << GUARD_OWNER_SHIFT);
}
uint32_t guard_owner(uint64_t word) {
  return static_cast<uint32_t>(word >> GUARD_OWNER_SHIFT) &
         GUARD_OWNER_MASK;
}

} /* namespace */

int __cxa_guard_acquire(guard_t* guard) {
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
      if (guard_owner(word) == guard_owner_bits(self)) {
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

void __cxa_guard_release(guard_t* guard) {
  (void)pthread_mutex_lock(&guard_mutex);
  guard_store(guard, GUARD_INITIALIZED);
  (void)pthread_cond_broadcast(&guard_cond);
  (void)pthread_mutex_unlock(&guard_mutex);
}

void __cxa_guard_abort(guard_t* guard) {
  const int32_t self = lw_current_tid();
  (void)pthread_mutex_lock(&guard_mutex);
  const uint64_t word = guard_load(guard);
  if ((word & 0xffu) == GUARD_INITIALIZING &&
      guard_owner(word) == guard_owner_bits(self)) {
    guard_store(guard, GUARD_UNINITIALIZED);
    (void)pthread_cond_broadcast(&guard_cond);
  }
  /* Aborting a guard this thread does not own is a caller bug the
   * ABI leaves undefined; the defensive choice is to leave the
   * state untouched rather than corrupt another thread's init. */
  (void)pthread_mutex_unlock(&guard_mutex);
}

} /* namespace __cxxabiv1 */
