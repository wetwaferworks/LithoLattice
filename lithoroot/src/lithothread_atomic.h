/* lithothread_atomic.h ... LithoThread's atomic shim (internal).
 *
 * Same shape and same reasoning as the weave's shim (the pieces
 * keep their own copies; the weave's header rules that no other
 * piece includes it): LithoThread needs load, store, exchange,
 * fetch-add, fetch-sub and compare-exchange on 32-bit words in
 * shared memory, all sequentially consistent. Seq-cst everywhere
 * is a deliberate simplicity choice: the primitive counts here
 * are small, the words are contended rarely, and the correctness
 * arguments in docs/LITHOTHREAD.md are stated against the
 * strongest model so they port unchanged to the wasm atomic
 * instructions (which is what the GCC / Clang builtins lower to
 * on wasm32 with atomics enabled).
 *
 *   - GCC / Clang: __atomic builtins.
 *   - MSVC: the Interlocked family.
 *   - Anything else: a C11 <stdatomic.h> fallback.
 *
 * Internal to LithoThread: the public header stays free of it.
 */
#ifndef LITHOTHREAD_ATOMIC_H
#define LITHOTHREAD_ATOMIC_H

#include <stdint.h>

#if defined(_MSC_VER)

#include <intrin.h>

static __inline uint32_t lt_at_load32(const volatile uint32_t* p) {
  return (uint32_t)_InterlockedCompareExchange((volatile long*)p, 0, 0);
}
static __inline void lt_at_store32(volatile uint32_t* p, uint32_t v) {
  (void)_InterlockedExchange((volatile long*)p, (long)v);
}
static __inline uint32_t lt_at_exchange32(volatile uint32_t* p, uint32_t v) {
  return (uint32_t)_InterlockedExchange((volatile long*)p, (long)v);
}
static __inline uint32_t lt_at_fetch_add32(volatile uint32_t* p, uint32_t v) {
  return (uint32_t)_InterlockedExchangeAdd((volatile long*)p, (long)v);
}
static __inline uint32_t lt_at_fetch_sub32(volatile uint32_t* p, uint32_t v) {
  return (uint32_t)_InterlockedExchangeAdd((volatile long*)p, -(long)v);
}
static __inline int lt_at_cas32(volatile uint32_t* p, uint32_t expected,
                                uint32_t desired) {
  return (uint32_t)_InterlockedCompareExchange((volatile long*)p,
                                               (long)desired,
                                               (long)expected) == expected;
}
static __inline void lt_at_spin_pause(void) { _mm_pause(); }

#elif defined(__GNUC__) || defined(__clang__)

static inline uint32_t lt_at_load32(const volatile uint32_t* p) {
  return __atomic_load_n((const uint32_t*)p, __ATOMIC_SEQ_CST);
}
static inline void lt_at_store32(volatile uint32_t* p, uint32_t v) {
  __atomic_store_n((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t lt_at_exchange32(volatile uint32_t* p, uint32_t v) {
  return __atomic_exchange_n((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t lt_at_fetch_add32(volatile uint32_t* p, uint32_t v) {
  return __atomic_fetch_add((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t lt_at_fetch_sub32(volatile uint32_t* p, uint32_t v) {
  return __atomic_fetch_sub((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline int lt_at_cas32(volatile uint32_t* p, uint32_t expected,
                              uint32_t desired) {
  return __atomic_compare_exchange_n((uint32_t*)p, &expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline void lt_at_spin_pause(void) {
#if defined(__i386__) || defined(__x86_64__)
  __builtin_ia32_pause();
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

#else /* fallback: C11 atomics */

#include <stdatomic.h>

static inline uint32_t lt_at_load32(const volatile uint32_t* p) {
  return atomic_load_explicit((const _Atomic(uint32_t)*)p,
                              memory_order_seq_cst);
}
static inline void lt_at_store32(volatile uint32_t* p, uint32_t v) {
  atomic_store_explicit((_Atomic(uint32_t)*)p, v, memory_order_seq_cst);
}
static inline uint32_t lt_at_exchange32(volatile uint32_t* p, uint32_t v) {
  return atomic_exchange_explicit((_Atomic(uint32_t)*)p, v,
                                  memory_order_seq_cst);
}
static inline uint32_t lt_at_fetch_add32(volatile uint32_t* p, uint32_t v) {
  return atomic_fetch_add_explicit((_Atomic(uint32_t)*)p, v,
                                   memory_order_seq_cst);
}
static inline uint32_t lt_at_fetch_sub32(volatile uint32_t* p, uint32_t v) {
  return atomic_fetch_sub_explicit((_Atomic(uint32_t)*)p, v,
                                   memory_order_seq_cst);
}
static inline int lt_at_cas32(volatile uint32_t* p, uint32_t expected,
                              uint32_t desired) {
  return atomic_compare_exchange_strong_explicit(
      (_Atomic(uint32_t)*)p, &expected, desired, memory_order_seq_cst,
      memory_order_seq_cst);
}
static inline void lt_at_spin_pause(void) {}

#endif

/* ThreadSanitizer teaching annotations. TSan models the host's
 * pthread primitives, not ours, so data protected only by a
 * LithoThread mutex would read as racy even when the locking is
 * correct. When (and only when) the file is built under TSan we
 * emit the acquire / release annotations TSan documents for
 * user-mode synchronization, at exactly the points the matching
 * host primitive would synchronize. Everywhere else these are
 * nothing at all. */
#if defined(__SANITIZE_THREAD__)
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
#define LT_TSAN_ACQUIRE(p) __tsan_acquire((void*)(p))
#define LT_TSAN_RELEASE(p) __tsan_release((void*)(p))
#else
#define LT_TSAN_ACQUIRE(p) ((void)0)
#define LT_TSAN_RELEASE(p) ((void)0)
#endif

#endif /* LITHOTHREAD_ATOMIC_H */
