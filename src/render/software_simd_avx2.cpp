// Cortana 2026-10-15 1455: AVX2 kernels (ENGINE). Only reached through
// software_simd.cpp after a CPUID/XGETBV check. GCC/Clang: per-function
// target("avx2") - this TU needs no -mavx2, so nothing else in the binary
// can accidentally use AVX2. MSVC: intrinsics compile without /arch.
#include <cstddef>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#include "litholattice/render/software_simd.h"
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline int llPopc(int v) { return int(__popcnt(unsigned(v))); }
#else
static inline int llPopc(int v) { return __builtin_popcount(unsigned(v)); }
#endif
#if defined(__GNUC__) || defined(__clang__)
#define LL_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define LL_TARGET_AVX2
#endif
namespace litholattice::swsimd {
LL_TARGET_AVX2 void fill32Avx2(uint32_t* p, size_t n, uint32_t v) {
  const __m256i vv = _mm256_set1_epi32(int(v));
  size_t i = 0;
  for (; i + 8 <= n; i += 8) _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + i), vv);
  for (; i < n; ++i) p[i] = v;
}
LL_TARGET_AVX2 void fillFAvx2(float* p, size_t n, float v) {
  const __m256 vv = _mm256_set1_ps(v);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) _mm256_storeu_ps(p + i, vv);
  for (; i < n; ++i) p[i] = v;
}

// Ginny 2026-10-16 (perf list, Cortana): 8-wide basic pixel shading for one row span.
// Bit-exact with the scalar loop in rasterizeTriangleShaded + basicFragmentShader:
//  - edge/depth lanes come from the SAME sequential scalar adds as the scalar loop;
//  - every expression keeps the scalar evaluation order ((a*x + b*y) + c*z), mul and add
//    as separate IEEE ops (target("avx2") does not enable FMA; MSVC never contracts here);
//  - float->int conversions truncate (cvtt) exactly like the scalar casts.
// swTile: t > 1 ? t - float(int(t)) : t  (lambdas would not inherit target("avx2"))
LL_TARGET_AVX2 static inline __m256 avxTile(__m256 t, __m256 one) {
  const __m256 fr = _mm256_sub_ps(t, _mm256_cvtepi32_ps(_mm256_cvttps_epi32(t)));
  return _mm256_blendv_ps(t, fr, _mm256_cmp_ps(t, one, _CMP_GT_OQ));
}
// Pass 2 step 4 (Ginny/Argon): spread a 4-bit value to the even bits (Morton)
LL_TARGET_AVX2 static inline __m256i avxSpread4(__m256i v) {
  v = _mm256_and_si256(_mm256_or_si256(v, _mm256_slli_epi32(v, 2)), _mm256_set1_epi32(0x33));
  return _mm256_and_si256(_mm256_or_si256(v, _mm256_slli_epi32(v, 1)), _mm256_set1_epi32(0x55));
}
// uint8_t(channel * light), channel = (texel >> sh) & 255
LL_TARGET_AVX2 static inline __m256i avxChan(__m256i texel, int sh, __m256 light, __m256i m255) {
  const __m256i c = _mm256_and_si256(_mm256_srl_epi32(texel, _mm_cvtsi32_si128(sh)), m255);
  return _mm256_and_si256(_mm256_cvttps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(c), light)), m255);
}
LL_TARGET_AVX2 void shadeSpanBasicAvx2(ShadeSpanBasic& s) {
  float e0 = s.sw0, e1 = s.sw1, e2 = s.sw2, ez = s.sdepth;
  const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f), k15 = _mm256_set1_ps(s.texScale);  // Cortana 2026-10-16: runtime tile
  const __m256 iw0 = _mm256_set1_ps(s.iw0), iw1 = _mm256_set1_ps(s.iw1), iw2 = _mm256_set1_ps(s.iw2);
  __m256 at[3][5];
  for (int v = 0; v < 3; ++v) for (int c = 0; c < 5; ++c) at[v][c] = _mm256_set1_ps(s.a[v][c]);
  // Argon point 3 (2026-10-18): per-vertex AO, interpolated + applied in-kernel
  // with the scalar loop's exact op order (see shadeSpanBasicAvx2 body).
  const __m256 ao0v = _mm256_set1_ps(s.ao[0]), ao1v = _mm256_set1_ps(s.ao[1]), ao2v = _mm256_set1_ps(s.ao[2]);
  const __m256 cx = _mm256_set1_ps(0.44f), cy = _mm256_set1_ps(0.88f), cz = _mm256_set1_ps(0.26f);
  const __m256 amb = _mm256_set1_ps(0.35f), dif = _mm256_set1_ps(0.65f);
  const __m256i m15 = _mm256_set1_epi32((1 << s.texShift) - 1), m255 = _mm256_set1_epi32(255), alpha = _mm256_set1_epi32(int(0xFF000000u));
  const __m256i laneIdx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  int wxLo = 1 << 30, wxHi = -1;
  uint64_t lc = 0, ls = 0;
  alignas(32) float L0[8], L1[8], L2[8], LZ[8];
  for (int base = 0; base < s.n; base += 8) {
    const int cnt = s.n - base < 8 ? s.n - base : 8;
    for (int j = 0; j < 8; ++j) {  // sequential stepping == scalar loop's running sums
      L0[j] = e0; L1[j] = e1; L2[j] = e2; LZ[j] = ez;
      e0 += s.dw0dx; e1 += s.dw1dx; e2 += s.dw2dx; ez += s.dzdx;
    }
    const __m256i live = _mm256_cmpgt_epi32(_mm256_set1_epi32(cnt), laneIdx);
    const __m256 w0 = _mm256_load_ps(L0), w1 = _mm256_load_ps(L1), w2 = _mm256_load_ps(L2), z = _mm256_load_ps(LZ);
    __m256 cov = _mm256_and_ps(_mm256_and_ps(_mm256_cmp_ps(w0, zero, _CMP_GE_OQ), _mm256_cmp_ps(w1, zero, _CMP_GE_OQ)),
                               _mm256_cmp_ps(w2, zero, _CMP_GE_OQ));
    cov = _mm256_and_ps(cov, _mm256_castsi256_ps(live));
    const int covBits = _mm256_movemask_ps(cov);
    if (!covBits) continue;
    lc += uint64_t(llPopc(covBits));
    uint16_t* drow = s.depth + base;
    // PT4-A: the depth row is uint16 (quantized). Pixels are paired into
    // 32-bit lanes; a pair is loaded/stored wide ONLY when both of its pixels
    // lie inside this span (lane < cnt), so a wide access never touches x1+1
    // (another strip/tile's pixel, possibly another thread's) or the end of
    // depth_. An odd last pixel (cnt odd) is read and written as a single
    // uint16. Within a full pair, an uncovered/failed pixel is written back
    // with its own just-loaded value; that pixel belongs to this span, so
    // only this thread touches it. Test in the integer domain with the shared
    // quantizer's rounding (cvtps == nearbyintf), bit-for-bit with scalar/span4.
    // (Cortana 2026-10-26, JD 12:03 PM: fixed the odd-pixel spill.)
    const int fullPairs = cnt >> 1;           // pairs wholly inside the span
    const int tail = (cnt & 1) ? cnt - 1 : -1; // odd last pixel, scalar
    auto pairMask = [fullPairs](int bits) {
      return _mm_setr_epi32((fullPairs > 0 && (bits & 0x03)) ? -1 : 0, (fullPairs > 1 && (bits & 0x0C)) ? -1 : 0,
                            (fullPairs > 2 && (bits & 0x30)) ? -1 : 0, (fullPairs > 3 && (bits & 0xC0)) ? -1 : 0);
    };
    __m128i draw = _mm_maskload_epi32(reinterpret_cast<const int*>(drow), pairMask(covBits));
    if (tail >= 0 && ((covBits >> tail) & 1)) {
      alignas(16) uint16_t dq[8];
      _mm_store_si128(reinterpret_cast<__m128i*>(dq), draw);
      dq[tail] = drow[tail];
      draw = _mm_load_si128(reinterpret_cast<const __m128i*>(dq));
    }
    const __m256i dstored = _mm256_cvtepu16_epi32(draw);
    __m256 zc = _mm256_min_ps(_mm256_max_ps(z, zero), one);
    if (swsimd::g_depthRemap) zc = _mm256_sub_ps(one, _mm256_sqrt_ps(_mm256_sub_ps(one, zc)));  // LL_SW_DEPTH_REMAP
    const __m256i zq = _mm256_cvtps_epi32(_mm256_mul_ps(zc, _mm256_set1_ps(65535.0f)));
    const __m256 pass = _mm256_and_ps(cov, _mm256_castsi256_ps(_mm256_cmpgt_epi32(dstored, zq)));
    const int passBits = _mm256_movemask_ps(pass);
    if (!passBits) continue;
    ls += uint64_t(llPopc(passBits));
    const __m256 pw0 = _mm256_mul_ps(w0, iw0), pw1 = _mm256_mul_ps(w1, iw1), pw2 = _mm256_mul_ps(w2, iw2);
    const __m256 vd = _mm256_div_ps(one, _mm256_add_ps(_mm256_add_ps(pw0, pw1), pw2));
    const __m256 q0 = _mm256_mul_ps(pw0, vd), q1 = _mm256_mul_ps(pw1, vd), q2 = _mm256_mul_ps(pw2, vd);
    __m256 att[5];
    for (int c = 0; c < 5; ++c)
      att[c] = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(q0, at[0][c]), _mm256_mul_ps(q1, at[1][c])), _mm256_mul_ps(q2, at[2][c]));
    // Scalar loop: a = ((w0*ao0 + w1*ao1) + w2*ao2), then per channel
    // uint32_t(float(channel) * a). Same association, same truncations here.
    const __m256 attAo = s.hasAo ? _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(q0, ao0v), _mm256_mul_ps(q1, ao1v)),
                                                 _mm256_mul_ps(q2, ao2v))
                                 : zero;
    const __m256i tx = _mm256_and_si256(_mm256_cvttps_epi32(_mm256_mul_ps(avxTile(att[3], one), k15)), m15);
    const __m256i ty = _mm256_and_si256(_mm256_cvttps_epi32(_mm256_mul_ps(avxTile(att[4], one), k15)), m15);
    // Pass 2 step 4 (Ginny/Argon): Morton layer index = spread(tx) | spread(ty) << 1
    const __m256i ti = s.morton ? _mm256_or_si256(avxSpread4(tx), _mm256_slli_epi32(avxSpread4(ty), 1))
                                : _mm256_add_epi32(_mm256_sll_epi32(ty, _mm_cvtsi32_si128(s.texShift)), tx);
    const __m256i texel = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), reinterpret_cast<const int*>(s.tex), ti,
                                                      _mm256_castps_si256(pass), 4);
    __m256 nl = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(att[0], cx), _mm256_mul_ps(att[1], cy)), _mm256_mul_ps(att[2], cz));
    nl = _mm256_blendv_ps(nl, zero, _mm256_cmp_ps(nl, zero, _CMP_LT_OQ));  // if (nl < 0) nl = 0
    const __m256 light = _mm256_add_ps(amb, _mm256_mul_ps(dif, nl));
    __m256i chR = avxChan(texel, 16, light, m255), chG = avxChan(texel, 8, light, m255), chB = avxChan(texel, 0, light, m255);
    if (s.hasAo) {  // post-shader AO scale, op for op with the scalar loop
      chR = _mm256_and_si256(_mm256_cvttps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(chR), attAo)), m255);
      chG = _mm256_and_si256(_mm256_cvttps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(chG), attAo)), m255);
      chB = _mm256_and_si256(_mm256_cvttps_epi32(_mm256_mul_ps(_mm256_cvtepi32_ps(chB), attAo)), m255);
    }
    const __m256i color = _mm256_or_si256(_mm256_or_si256(alpha, _mm256_slli_epi32(chR, 16)),
                                          _mm256_or_si256(_mm256_slli_epi32(chG, 8), chB));
    const __m256i pm = _mm256_castps_si256(pass);
    // PT4-A: merged depths: pass lanes take zq, the rest keep the stored
    // value; packed to 8x u16. Full in-span pairs are stored wide; the odd
    // last pixel is stored as one uint16 (see the load above).
    const __m256i dmerged = _mm256_blendv_epi8(dstored, zq, pm);
    const __m128i dpack = _mm_packus_epi32(_mm256_castsi256_si128(dmerged), _mm256_extracti128_si256(dmerged, 1));
    _mm_maskstore_epi32(reinterpret_cast<int*>(drow), pairMask(passBits), dpack);
    if (tail >= 0 && ((passBits >> tail) & 1)) {
      alignas(16) uint16_t dq[8];
      _mm_store_si128(reinterpret_cast<__m128i*>(dq), dpack);
      drow[tail] = dq[tail];
    }
    _mm256_maskstore_epi32(reinterpret_cast<int*>(s.pixels + base), pm, color);
    int lo = 0; while (!((passBits >> lo) & 1)) ++lo;
    int hi = 7; while (!((passBits >> hi) & 1)) --hi;
    if (base + lo < wxLo) wxLo = base + lo;
    wxHi = base + hi;
  }
  s.wxLo = wxHi >= 0 ? wxLo : 0; s.wxHi = wxHi; s.lc = lc; s.ls = ls;
}
}  // namespace litholattice::swsimd
#endif