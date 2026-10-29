/* lithoabi_atomic.h ... LithoAbi's atomic shim (internal).
 *
 * Same house pattern as LithoWeave's and LithoThread's shims: the
 * operations LithoAbi needs on 32-bit words (exception refcounts,
 * the emergency pool's slot bitmap, guard fast-path loads), at the
 * strongest model, so the code ports unchanged to the wasm atomic
 * instructions. GCC / Clang use __atomic builtins; anything else
 * falls back to C11 <stdatomic.h>.
 */
#ifndef LITHOABI_ATOMIC_H
#define LITHOABI_ATOMIC_H

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)

static inline uint32_t la_atomic_load32(const uint32_t* p) {
  return __atomic_load_n((const uint32_t*)p, __ATOMIC_SEQ_CST);
}
static inline void la_atomic_store32(uint32_t* p, uint32_t v) {
  __atomic_store_n((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t la_atomic_fetch_add32(uint32_t* p, uint32_t v) {
  return __atomic_fetch_add((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t la_atomic_fetch_sub32(uint32_t* p, uint32_t v) {
  return __atomic_fetch_sub((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline int la_atomic_cas32(uint32_t* p, uint32_t* expected,
                                  uint32_t desired) {
  return __atomic_compare_exchange_n((uint32_t*)p, expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline uint32_t la_atomic_exchange32(uint32_t* p, uint32_t v) {
  return __atomic_exchange_n((uint32_t*)p, v, __ATOMIC_SEQ_CST);
}
static inline uint64_t la_atomic_load64(const uint64_t* p) {
  return __atomic_load_n((const uint64_t*)p, __ATOMIC_SEQ_CST);
}
static inline void la_atomic_store64(uint64_t* p, uint64_t v) {
  __atomic_store_n((uint64_t*)p, v, __ATOMIC_SEQ_CST);
}

#else /* fallback: C11 atomics */

#include <stdatomic.h>

static inline uint32_t la_atomic_load32(const uint32_t* p) {
  return atomic_load_explicit((const _Atomic(uint32_t)*)p,
                              memory_order_seq_cst);
}
static inline void la_atomic_store32(uint32_t* p, uint32_t v) {
  atomic_store_explicit((_Atomic(uint32_t)*)p, v, memory_order_seq_cst);
}
static inline uint32_t la_atomic_fetch_add32(uint32_t* p, uint32_t v) {
  return atomic_fetch_add_explicit((_Atomic(uint32_t)*)p, v,
                                   memory_order_seq_cst);
}
static inline uint32_t la_atomic_fetch_sub32(uint32_t* p, uint32_t v) {
  return atomic_fetch_sub_explicit((_Atomic(uint32_t)*)p, v,
                                   memory_order_seq_cst);
}
static inline int la_atomic_cas32(uint32_t* p, uint32_t* expected,
                                  uint32_t desired) {
  return atomic_compare_exchange_strong_explicit(
      (_Atomic(uint32_t)*)p, expected, desired, memory_order_seq_cst,
      memory_order_seq_cst);
}
static inline uint32_t la_atomic_exchange32(uint32_t* p, uint32_t v) {
  return atomic_exchange_explicit((_Atomic(uint32_t)*)p, v,
                                  memory_order_seq_cst);
}
static inline uint64_t la_atomic_load64(const uint64_t* p) {
  return atomic_load_explicit((const _Atomic(uint64_t)*)p,
                              memory_order_seq_cst);
}
static inline void la_atomic_store64(uint64_t* p, uint64_t v) {
  atomic_store_explicit((_Atomic(uint64_t)*)p, v, memory_order_seq_cst);
}

#endif

#endif /* LITHOABI_ATOMIC_H */
