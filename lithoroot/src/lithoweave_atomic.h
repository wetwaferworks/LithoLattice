/* lithoweave_atomic.h ... the weave's tiny atomic shim (internal).
 *
 * LithoWeave needs exactly four operations on 32-bit words in
 * shared memory: load, store, fetch-add and exchange, all
 * sequentially consistent (the tid cell protocol is specified in
 * terms of a single seq-cst store observed by seq-cst loads and
 * waits, matching both the wasm atomic instructions and JS
 * Atomics). Rather than depend on a given compiler's C11 atomics
 * support in strict C99 mode (uneven across MSVC, strict-pedantic
 * GCC and the wasm toolchain), the operations are spelled once
 * here over whatever the compiler at hand provides:
 *
 *   - GCC / Clang (including the wasm target): __atomic builtins,
 *     which lower to the wasm memory.atomic instructions on
 *     wasm32 with atomics enabled and to locked / native atomic
 *     instructions on native targets.
 *   - MSVC: the Interlocked family.
 *   - Anything else: a C11 <stdatomic.h> fallback over
 *     _Atomic(uint32_t) casts.
 *
 * This header is internal to LithoWeave: the public header stays
 * free of it, and no other piece includes it.
 */
#ifndef LITHOWEAVE_ATOMIC_H
#define LITHOWEAVE_ATOMIC_H

#include <stdint.h>

#if defined(_MSC_VER)

#include <intrin.h>

static __inline uint32_t lw_at_load32(const volatile uint32_t* p) {
  return (uint32_t)_InterlockedCompareExchange((volatile long*)p, 0, 0);
}
static __inline void lw_at_store32(volatile uint32_t* p, uint32_t v) {
  (void)_InterlockedExchange((volatile long*)p, (long)v);
}
static __inline uint32_t lw_at_fetch_add32(volatile uint32_t* p, uint32_t v) {
  return (uint32_t)_InterlockedExchangeAdd((volatile long*)p, (long)v);
}
static __inline uint32_t lw_at_exchange32(volatile uint32_t* p, uint32_t v) {
  return (uint32_t)_InterlockedExchange((volatile long*)p, (long)v);
}
/* CAS: 1 and the word replaced when *expected matched, else 0 with
 * *expected refreshed. The registry claims slots with this. */
static __inline int lw_at_cas32(volatile uint32_t* p, uint32_t* expected, uint32_t desired) {
  uint32_t seen = (uint32_t)_InterlockedCompareExchange((volatile long*)p, (long)desired, (long)*expected);
  if (seen == *expected) return 1;
  *expected = seen;
  return 0;
}

#elif defined(__GNUC__) || defined(__clang__)

static inline uint32_t lw_at_load32(const volatile uint32_t* p) {
  return __atomic_load_n((const uint32_t*)p, __ATOMIC_SEQ_CST);
}
static inline void lw_at_store32(volatile uint32_t* p, uint32_t v) {
  __atomic_store_n((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t lw_at_fetch_add32(volatile uint32_t* p, uint32_t v) {
  return __atomic_fetch_add((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t lw_at_exchange32(volatile uint32_t* p, uint32_t v) {
  return __atomic_exchange_n((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline int lw_at_cas32(volatile uint32_t* p, uint32_t* expected, uint32_t desired) {
  return __atomic_compare_exchange_n((uint32_t*)p, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#else /* fallback: C11 atomics */

#include <stdatomic.h>

static inline uint32_t lw_at_load32(const volatile uint32_t* p) {
  return atomic_load_explicit((const _Atomic(uint32_t)*)p, memory_order_seq_cst);
}
static inline void lw_at_store32(volatile uint32_t* p, uint32_t v) {
  atomic_store_explicit((_Atomic(uint32_t)*)p, v, memory_order_seq_cst);
}
static inline uint32_t lw_at_fetch_add32(volatile uint32_t* p, uint32_t v) {
  return atomic_fetch_add_explicit((_Atomic(uint32_t)*)p, v, memory_order_seq_cst);
}
static inline uint32_t lw_at_exchange32(volatile uint32_t* p, uint32_t v) {
  return atomic_exchange_explicit((_Atomic(uint32_t)*)p, v, memory_order_seq_cst);
}
static inline int lw_at_cas32(volatile uint32_t* p, uint32_t* expected, uint32_t desired) {
  return atomic_compare_exchange_strong_explicit((_Atomic(uint32_t)*)p, expected, desired, memory_order_seq_cst, memory_order_seq_cst);
}

#endif

#endif /* LITHOWEAVE_ATOMIC_H */
