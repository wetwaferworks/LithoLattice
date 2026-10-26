// Cortana 2026-10-15 1455: SIMD dispatch (see software_simd.h). ENGINE.
// Ginny (Argon) 2026-10-16 Pass 2 step 5 (Cortana): tiered runtime dispatch. One CPUID check at
// first use picks scalar / SSE2 / SSE4.1 / AVX2; every kernel lives in its own translation unit
// (software_simd_sse2.cpp, software_simd_sse41.cpp, software_simd_avx2.cpp) and this file only
// routes. Every tier is bit-identical to the scalar code.
#include "litholattice/render/software_simd.h"
#include <cstdlib>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#define LL_SW_X64 1
#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <cpuid.h>
#endif
#endif
#if defined(LL_SW_AVX2_TU) && defined(LL_SW_X64)
#define LL_SW_HAVE_AVX2_TU 1
#endif
#if defined(LL_SW_SSE_TU) && defined(LL_SW_X64)
#define LL_SW_HAVE_SSE_TU 1
#endif

namespace litholattice::swsimd {
#ifdef LL_SW_HAVE_AVX2_TU
void fill32Avx2(uint32_t* p, size_t n, uint32_t v);  // software_simd_avx2.cpp
void fillFAvx2(float* p, size_t n, float v);
void shadeSpanBasicAvx2(ShadeSpanBasic& s);  // Ginny 2026-10-16
#endif
#ifdef LL_SW_HAVE_SSE_TU
void fill32Sse2(uint32_t* p, size_t n, uint32_t v);  // software_simd_sse2.cpp
void fillFSse2(float* p, size_t n, float v);
void shadeSpanBasicSse2(ShadeSpanBasic& s);
void transformQuadSse2(const uint16_t* xyz, size_t strideU16, const float* m, float* out, float W, float H);
void shadeSpanBasicSse41(ShadeSpanBasic& s);  // software_simd_sse41.cpp
void transformQuadSse41(const uint16_t* xyz, size_t strideU16, const float* m, float* out, float W, float H);
#endif

#ifdef LL_SW_X64
static void cpuidx(int leaf, int sub, unsigned r[4]) {
#if defined(_MSC_VER)
  int x[4];
  __cpuidex(x, leaf, sub);
  for (int i = 0; i < 4; ++i) r[i] = unsigned(x[i]);
#else
  __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}
static Isa detectIsa() {
  unsigned r[4];
  cpuidx(0, 0, r);
  const unsigned maxLeaf = r[0];
  cpuidx(1, 0, r);
  Isa best = Isa::Sse2;  // architectural on x86-64
  if ((r[2] >> 19) & 1) best = Isa::Sse41;
  const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
  if (best == Isa::Sse41 && maxLeaf >= 7 && osxsave && avx) {
#if defined(_MSC_VER)
    const unsigned long long xcr0 = _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    const unsigned long long xcr0 = lo;
#endif
    if ((xcr0 & 6) == 6) {  // OS saves XMM+YMM
      cpuidx(7, 0, r);
      if ((r[1] >> 5) & 1) best = Isa::Avx2;
    }
  }
  return best;
}
#endif

Isa activeIsa() {
  static const Isa isa = [] {
    const char* f = std::getenv("LL_SW_FORCE_SCALAR");
    if (f && *f && *f != '0') return Isa::Scalar;
#ifdef LL_SW_X64
    Isa i = detectIsa();
#ifndef LL_SW_HAVE_AVX2_TU
    if (i == Isa::Avx2) i = Isa::Sse41;
#endif
#ifndef LL_SW_HAVE_SSE_TU
    if (i != Isa::Avx2) i = Isa::Scalar;
#endif
    if (const char* cap = std::getenv("LL_SW_ISA")) {
      Isa c = i;
      if (!std::strcmp(cap, "scalar")) c = Isa::Scalar;
      else if (!std::strcmp(cap, "sse2")) c = Isa::Sse2;
      else if (!std::strcmp(cap, "sse41")) c = Isa::Sse41;
      else if (!std::strcmp(cap, "avx2")) c = Isa::Avx2;
      if (int(c) < int(i)) i = c;
#ifndef LL_SW_HAVE_SSE_TU
      if (i != Isa::Avx2) i = Isa::Scalar;
#endif
    }
    return i;
#else
    return Isa::Scalar;
#endif
  }();
  return isa;
}
bool avx2Active() { return activeIsa() == Isa::Avx2; }
const char* activePath() {
  switch (activeIsa()) {
    case Isa::Avx2: return "avx2";
    case Isa::Sse41: return "sse4.1";
    case Isa::Sse2: return "sse2";
    default: return "scalar";
  }
}

void fill32(uint32_t* p, size_t n, uint32_t v) {
#ifdef LL_SW_HAVE_AVX2_TU
  if (activeIsa() == Isa::Avx2) { fill32Avx2(p, n, v); return; }
#endif
#ifdef LL_SW_HAVE_SSE_TU
  if (activeIsa() >= Isa::Sse2) { fill32Sse2(p, n, v); return; }
#endif
  for (size_t i = 0; i < n; ++i) p[i] = v;
}
void fillF(float* p, size_t n, float v) {
#ifdef LL_SW_HAVE_AVX2_TU
  if (activeIsa() == Isa::Avx2) { fillFAvx2(p, n, v); return; }
#endif
#ifdef LL_SW_HAVE_SSE_TU
  if (activeIsa() >= Isa::Sse2) { fillFSse2(p, n, v); return; }
#endif
  for (size_t i = 0; i < n; ++i) p[i] = v;
}
/// PT4-A: 16-bit fill built on the 32-bit pattern fill (two pixels per
/// word) plus a scalar tail for odd counts.
void fill16(uint16_t* p, size_t n, uint16_t v) {
  const uint32_t pat = uint32_t(v) | (uint32_t(v) << 16);
  fill32(reinterpret_cast<uint32_t*>(p), n / 2, pat);
  if (n & 1) p[n - 1] = v;
}

bool shadeSpanEnabled() {
  static const bool on = [] {
    // Strike 1 (Elfie bench, 2026-10-26, Archie battle plan): SIMD span shading is
    // DEFAULT ON when the CPU has a SIMD ISA; LL_SW_SIMD_SHADE=0 is the explicit
    // opt-out (A/B proof path). Previously opt-in via LL_SW_SIMD_SHADE=1, which no
    // default run ever set — the profiler read SIMD 0.0% of span pixels.
    const char* f = std::getenv("LL_SW_SIMD_SHADE");
    if (f && *f == '0') return false;
    return activeIsa() != Isa::Scalar;
  }();
  return on;
}
bool shadeSpanBasic(ShadeSpanBasic& s) {  // Ginny 2026-10-16 (perf list, Cortana)
  switch (activeIsa()) {
#ifdef LL_SW_HAVE_AVX2_TU
    case Isa::Avx2: shadeSpanBasicAvx2(s); return true;
#endif
#ifdef LL_SW_HAVE_SSE_TU
    case Isa::Sse41: shadeSpanBasicSse41(s); return true;
    case Isa::Sse2: shadeSpanBasicSse2(s); return true;
#endif
    default: (void)s; return false;
  }
}
TransformQuadFn transformQuadFn() {
#ifdef LL_SW_HAVE_SSE_TU
  switch (activeIsa()) {
    case Isa::Avx2:
    case Isa::Sse41: return &transformQuadSse41;
    case Isa::Sse2: return &transformQuadSse2;
    default: break;
  }
#endif
  return nullptr;
}
}  // namespace litholattice::swsimd
