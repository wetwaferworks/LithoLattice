// lk_simd_wasm_check.c ... bit-exactness check for lk_simd_wasm.h against real SSE2/SSE4.1.
// Build twice from this one file: x86-64 with <smmintrin.h> (-msse4.1), and wasm32 with
// lk_simd_wasm.h (-msimd128). Both write every intrinsic's result for the same edge-case
// vectors into out[]; the runner compares the two dumps word for word.
// Run (from engine/tests/web; result 2026-11-16: BIT-EXACT, 28,434 words, NaN/inf/+-0/out-of-range/half-way):
//   clang -O2 -msse4.1 lk_simd_wasm_check.c -o native_check && ./native_check > native.txt
//   clang --target=wasm32-unknown-unknown -O2 -msimd128 -nostdlib -ffreestanding -I../../src/render \
//     -I../../lithoroot/include -c lk_simd_wasm_check.c -o check.o
//   wasm-ld --no-entry --export=run --export=buf_addr --allow-undefined check.o -o check.wasm
//   (run check.wasm under node, dump buf words to wasm.txt; memcpy is its only import), then cmp native.txt wasm.txt
#include <stdint.h>
#include <string.h>
#if defined(__wasm__)
#include "lk_simd_wasm.h"
#else
#include <smmintrin.h>
#endif
static uint32_t* o;
static unsigned n;
static void put_ps(__m128 v) { float f[4]; _mm_storeu_ps(f, v); memcpy(o + n, f, 16); n += 4; }
static void put_si(__m128i v) { _mm_storeu_si128((__m128i*)(o + n), v); n += 4; }
static void put_i(int v) { o[n++] = (uint32_t)v; }
static float fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
unsigned run(uint32_t* out) {
  o = out; n = 0;
  const float F[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, -2.5f, -0.5f, 3.4999998f, 65535.0f, 1e-40f,
                     2147483520.0f, 2147483648.0f, -2147483648.0f, -2147483904.0f, 4e9f, -4e9f, 1e30f,
                     fb(0x7f800000u), fb(0xff800000u), fb(0x7fc00000u), fb(0xffc00001u), 123.456f, -7.75f, 15.999f};
  const unsigned NF = sizeof F / sizeof F[0];
  const int I[] = {0, 1, -1, 2, 255, 256, 65535, 65536, -65536, 70000, 0x7fffffff, (int)0x80000000, 12345, -12345, 31, 32};
  const unsigned NI = sizeof I / sizeof I[0];
  for (unsigned i = 0; i < NF; ++i)
    for (unsigned j = 0; j < NF; j += 3) {
      const __m128 a = _mm_set_ps(F[i], F[(i + 1) % NF], F[(i + 5) % NF], F[(i + 11) % NF]);
      const __m128 b = _mm_set_ps(F[j], F[(j + 7) % NF], F[(j + 2) % NF], F[i]);
      put_ps(_mm_add_ps(a, b)); put_ps(_mm_sub_ps(a, b)); put_ps(_mm_mul_ps(a, b)); put_ps(_mm_div_ps(a, b));
      put_ps(_mm_min_ps(a, b)); put_ps(_mm_max_ps(a, b));
      put_ps(_mm_cmplt_ps(a, b)); put_ps(_mm_cmpgt_ps(a, b)); put_ps(_mm_cmpge_ps(a, b));
      put_ps(_mm_and_ps(a, b)); put_ps(_mm_or_ps(a, b)); put_ps(_mm_andnot_ps(a, b));
      put_ps(_mm_blendv_ps(a, b, _mm_cmplt_ps(a, b))); put_ps(_mm_blendv_ps(a, b, a));
      put_i(_mm_movemask_ps(a));
    }
  for (unsigned i = 0; i < NF; ++i) {
    const __m128 a = _mm_set_ps(F[i], F[(i + 3) % NF], F[(i + 9) % NF], F[(i + 13) % NF]);
    put_ps(_mm_sqrt_ps(a)); put_si(_mm_cvttps_epi32(a)); put_si(_mm_cvtps_epi32(a));
    put_si(_mm_castps_si128(a)); put_ps(_mm_castsi128_ps(_mm_castps_si128(a)));
    put_ps(_mm_cvtepi32_ps(_mm_cvttps_epi32(a)));
    float al[4] __attribute__((aligned(16)));
    _mm_store_ps(al, a); put_ps(_mm_load_ps(al));
    __m128 r0 = a, r1 = _mm_set1_ps(F[i]), r2 = _mm_setzero_ps(), r3 = _mm_set_ps(1, 2, 3, 4);
    _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
    put_ps(r0); put_ps(r1); put_ps(r2); put_ps(r3);
  }
  for (unsigned i = 0; i < NI; ++i)
    for (unsigned j = 0; j < NI; ++j) {
      const __m128i a = _mm_set_epi32(I[i], I[(i + 1) % NI], I[(i + 4) % NI], I[(i + 9) % NI]);
      const __m128i b = _mm_set_epi32(I[j], I[(j + 3) % NI], I[(j + 6) % NI], I[i]);
      put_si(_mm_add_epi32(a, b)); put_si(_mm_sub_epi32(a, b)); put_si(_mm_and_si128(a, b)); put_si(_mm_or_si128(a, b));
      put_si(_mm_cmpgt_epi32(a, b)); put_si(_mm_packus_epi32(a, b)); put_si(_mm_unpacklo_epi16(a, b));
      put_si(_mm_sll_epi32(a, _mm_cvtsi32_si128(I[j] & 63)));
      put_si(_mm_cvtsi32_si128(I[j])); put_i(_mm_cvtsi128_si32(a));
      put_ps(_mm_cvtepi32_ps(a));
      __m128i st; _mm_store_si128(&st, a); put_si(st);
      uint32_t lo[4] = {(uint32_t)I[i], (uint32_t)I[j], 0xdeadbeefu, 0xdeadbeefu};
      put_si(_mm_loadl_epi64((const __m128i*)lo));
    }
  for (unsigned i = 0; i < NI; ++i) {
    const __m128i a = _mm_set_epi32(I[i], I[(i + 5) % NI], I[(i + 2) % NI], I[(i + 7) % NI]);
#define SH(k) put_si(_mm_slli_epi32(a, k)); put_si(_mm_srli_epi32(a, k));
    SH(0) SH(1) SH(8) SH(16) SH(31) SH(32) SH(40)
    put_si(_mm_shuffle_epi32(a, 3)); put_si(_mm_shuffle_epi32(a, 2)); put_si(_mm_shuffle_epi32(a, 1));
    put_si(_mm_shuffle_epi32(a, 0x1b)); put_si(_mm_shuffle_epi32(a, 0xe4));
    put_si(_mm_set1_epi32(I[i])); put_si(_mm_setzero_si128()); put_ps(_mm_set1_ps(F[i % NF]));
  }
  return n;
}
#if !defined(__wasm__)
#include <stdio.h>
static uint32_t buf[1 << 20];
int main(void) { unsigned k = run(buf); for (unsigned i = 0; i < k; ++i) printf("%08x\n", buf[i]); return 0; }
#else
uint32_t lk_buf[1 << 20];
uint32_t* buf_addr(void) { return lk_buf; }
#endif
