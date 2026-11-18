/* int128.c ... LithoRoot's compiler-builtin subset: 128-bit integer
 * multiply.
 *
 * clang lowers a 128-bit multiply on wasm32 to a call to __multi3
 * (LLVM's runtime-library contract, the same name compiler-rt's
 * builtins provide). LithoRoot links no compiler-rt and no external
 * runtime: this file is the in-house subset, and it carries only
 * what the engine's link actually asks for. Today that is __multi3
 * alone (main.cpp and world_save.cpp, through 64x64->128 hash and
 * seed arithmetic). Add a builtin here when a link reports it
 * missing, with a check in tests/rt_int128_check.c.
 *
 * The body must not itself multiply in 128 bits (that would lower
 * straight back into __multi3), so it works in 32-bit limbs with
 * 64-bit partial products, schoolbook style. The result is the low
 * 128 bits of the product, which is the same for signed and
 * unsigned operands in two's complement.
 */
#include <stdint.h>

typedef __int128 lr_ti_t;
typedef unsigned __int128 lr_uti_t;

static uint64_t lr__mul_64x64_hi(uint64_t a, uint64_t b, uint64_t* lo) {
  uint64_t a0 = (uint32_t)a, a1 = a >> 32;
  uint64_t b0 = (uint32_t)b, b1 = b >> 32;
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  *lo = (mid << 32) | (uint32_t)p00;
  return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

lr_ti_t __multi3(lr_ti_t a, lr_ti_t b) {
  lr_uti_t ua = (lr_uti_t)a, ub = (lr_uti_t)b;
  uint64_t alo = (uint64_t)ua, ahi = (uint64_t)(ua >> 64);
  uint64_t blo = (uint64_t)ub, bhi = (uint64_t)(ub >> 64);
  uint64_t lo;
  uint64_t hi = lr__mul_64x64_hi(alo, blo, &lo);
  hi += alo * bhi + ahi * blo; /* only the low 64 bits of the cross terms land */
  return (lr_ti_t)(((lr_uti_t)hi << 64) | lo);
}
