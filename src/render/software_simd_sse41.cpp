// Ginny (Argon) 2026-10-16 Pass 2 step 5 (Cortana): SSE4.1 kernels (ENGINE). GCC/Clang use
// per-function target("sse4.1") attributes (no -msse4.1 on the TU, so no SSE4.1 code can
// leak into inline functions shared with baseline code); MSVC compiles the intrinsics
// without /arch. Reached only after the CPUID check in software_simd.cpp.
#include <cstddef>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__)
#include <smmintrin.h>
#include "litholattice/render/software_simd.h"
#if defined(__GNUC__) || defined(__clang__)
#define LL_TARGET_SSE41 __attribute__((target("sse4.1")))
#else
#define LL_TARGET_SSE41
#endif
namespace litholattice::swsimd {
LL_TARGET_SSE41 static inline __m128 sse41Blend(__m128 a, __m128 b, __m128 m) { return _mm_blendv_ps(a, b, m); }
#define LL_SPAN4_FN shadeSpanBasicSse41
#define LL_QUAD4_FN transformQuadSse41
#define LL_SPAN4_TARGET LL_TARGET_SSE41
#define LL_SPAN4_LAMBDA_TARGET
#define LL_SPAN4_BLEND_PS(a, b, m) sse41Blend((a), (b), (m))
#include "software_simd_span4.inl"
}  // namespace litholattice::swsimd
#endif
