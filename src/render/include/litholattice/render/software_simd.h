// Cortana 2026-10-15 1455: software-renderer SIMD runtime dispatch.
// Baseline (SSE2 / NEON / scalar) code is always built; the AVX2 kernels
// live in software_simd_avx2.cpp with per-function target attributes and
// are chosen at first use only when CPUID + XGETBV report AVX2 with OS YMM
// support. LL_SW_FORCE_SCALAR=1 forces the baseline path (identity proofs).
// Every kernel has bit-identical output on both paths (pure stores).
#pragma once
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
namespace litholattice::swsimd {
/// True when the AVX2 path is compiled in, supported by CPU+OS, and not
/// disabled by LL_SW_FORCE_SCALAR.
bool avx2Active();
/// Ginny (Argon) 2026-10-16 Pass 2 step 5: the instruction-set tier picked ONCE at first use
/// from CPUID (+XGETBV for AVX2). LL_SW_ISA=scalar|sse2|sse41|avx2 caps it (testing old
/// CPUs on new ones); LL_SW_FORCE_SCALAR=1 = scalar.
enum class Isa : int { Scalar = 0, Sse2 = 1, Sse41 = 2, Avx2 = 3 };
Isa activeIsa();
/// Short name of the active path ("avx2" / "sse4.1" / "sse2" / "scalar").
const char* activePath();
/// Pass 2 step 5: 4-wide transform of one quad (4 PackedVertex-style records: uint16 x,y,z at
/// the start of each, `strideU16` uint16s apart) to screen space (x, y, depth, clipW per
/// vertex, 16 floats). Bit-identical to the scalar transformVertex. Null when scalar.
using TransformQuadFn = void (*)(const uint16_t* xyz, size_t strideU16, const float* mvp, float* out, float W, float H);
TransformQuadFn transformQuadFn();
/// p[0..n) = v (32-bit pattern). Used for colour + depth clears.
void fill32(uint32_t* p, size_t n, uint32_t v);
/// p[0..n) = v (float; depth clears). Bit-identical on both paths.
void fillF(float* p, size_t n, float v);
/// p[0..n) = v (16-bit pattern; depth clears). Bit-identical on both paths.
void fill16(uint16_t* p, size_t n, uint16_t v);

/// PT4 Target A (Elfie, 2026-10-26 — Archie PT4 brief): the depth buffer
/// is uint16_t, quantized from the [0,1]-domain float depth (0 = near,
/// 0xFFFF = far/clear). ONE quantizer for every path — renderer scalar
/// loops, the span4 kernels, the AVX2 kernel — so all tiers test
/// identically: a fragment passes iff depthQ(frag) < stored, and a write
/// stores depthQ(frag). Interpolation stays float end to end; the
/// quantization happens once per pixel, at the test/write. Rounding is
/// round-to-nearest-even: nearbyintf here, cvtps in the kernels — the
/// same rule by design.
/// Cortana 2026-10-26 (JD distant banding): LL_SW_DEPTH_REMAP=1 stores
/// 1 - sqrt(1 - d) instead of d. NDC depth with near 0.05 / far 1000 has
/// a 16-bit step of ~1.25 blocks at 64, ~5 at 128 and ~20 at 256, so far
/// faces one block apart quantize equal and z-fight (banding, rail moire).
/// The remap is monotonic (ordering kept) and spends codes on the far end:
/// ~0.07 blocks at 64, ~0.2 at 128, ~0.56 at 256. Opt-in for A/B.
/// Cortana 2026-10-26 (Archie item 7): LL_SW_DEPTH_REMAP=2 = reciprocal ("log") depth. With
/// a [0,1] NDC depth 1 - d ~ near/w, so the float BITS of u = 1 - d are a piecewise-linear
/// log2(near/w): codes = (bits(1) - bits(max(u, 2^-15))) / (15 * 2^23), i.e. 15 octaves of
/// w with ~4369 codes each = constant ~0.02% relative precision (0.06 blocks at 256, 0.23 at
/// 1000) instead of d's step that grows with w^2. Monotonic, integer-cheap, all tiers.
/// LL_RETRO_SHIMMER=1 (JD partly likes the Tron-like distant shimmer): keeps the original
/// linear 16-bit depth, so far faces keep their banding/shimmer whatever LL_SW_DEPTH_REMAP says.
inline const int g_depthRemapMode = [] {
  const char* r = std::getenv("LL_RETRO_SHIMMER");
  if (r && *r == '1') return 0;
  const char* e = std::getenv("LL_SW_DEPTH_REMAP"); return (e && (*e == '1' || *e == '2')) ? *e - '0' : 0; }();
inline const bool g_depthRemap = g_depthRemapMode != 0;
inline float depthRecip(float d) {
  float u = 1.0f - d;
  u = u < 3.0517578125e-05f ? 3.0517578125e-05f : u;
  uint32_t b; std::memcpy(&b, &u, 4);
  return float(int32_t(0x3F800000u - b)) * (1.0f / (15.0f * 8388608.0f));
}
inline float depthRemap(float d) { return g_depthRemapMode == 1 ? 1.0f - std::sqrt(1.0f - d) : g_depthRemapMode == 2 ? depthRecip(d) : d; }
inline uint16_t depthQ(float d) {
  d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
  d = depthRemap(d);
  return uint16_t(std::nearbyintf(d * 65535.0f));
}
/// Conservative bounds for integer-domain comparisons (Hi-Z skip test):
/// depthQFloor never overstates a minimum, depthQCeil never understates
/// a slack, so a quantized skip test fires only when the float test
/// would have fired too.
inline uint16_t depthQFloor(float d) {
  d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
  d = depthRemap(d);
  return uint16_t(std::floor(d * 65535.0f));
}
inline uint16_t depthQCeil(float d) {
  d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
  const float q = d * 65535.0f;
  const float f = std::floor(q);
  return uint16_t(f == q ? q : f + 1.0f);
}

/// Ginny 2026-10-16 (perf list, Cortana): one row span of the per-pixel chunk raster for
/// the common case (basic diffuse shading, plain texture, no normal/height map, no dither,
/// no ground FX, no strip/cutout/blend mode). Inputs mirror rasterizeTriangleShaded's
/// row state exactly; the AVX2 kernel reproduces the scalar arithmetic op-for-op (same
/// incremental edge stepping, same operation order, no FMA) so output is bit-identical.
struct ShadeSpanBasic {
  float sw0, sw1, sw2, sdepth;          ///< edge/depth values at pixel x0 (row start)
  float dw0dx, dw1dx, dw2dx, dzdx;      ///< per-pixel steps
  float iw0, iw1, iw2;                  ///< 1/w per vertex
  float a[3][5];                        ///< per vertex: nx, ny, nz, u, v
  float ao[3] = {1.0f, 1.0f, 1.0f};     ///< per-vertex AO factor (kSwAoLut output)
  bool hasAo = false;                   ///< any ao[k] < 1.0f: kernels apply the scalar
                                        ///< loop's post-shader AO scale, bit-identically
  const uint32_t* tex;                  ///< 16x16 texel layer (texData + layer*256)
  uint16_t* depth;                         ///< depth row at x0
  uint32_t* pixels;                     ///< colour row at x0
  int n;                                ///< pixels in the span (x1 - x0 + 1)
  bool morton;                          ///< Pass 2 step 4: tex layer is Morton-swizzled (16x16 only)
  int texShift = 4;                     ///< Cortana 2026-10-16: log2 tile size (4/5/6)
  float texScale = 15.0f;               ///< tile size - 1 (u * scale, truncated, & mask)
  // outputs
  int wxLo, wxHi;                       ///< written range (relative to x0), wxHi < 0 = none
  uint64_t lc, ls;                      ///< covered / depth-passed counts (LL_SW_PROF)
};
/// Runs the AVX2 span kernel. Returns false (and does nothing) when AVX2 is not active:
/// the caller then runs its scalar loop.
bool shadeSpanBasic(ShadeSpanBasic& s);
/// True when the AVX2 span kernel should be used: AVX2 active AND opted in with
/// LL_SW_SIMD_SHADE=1. Default OFF: bit-identical but measured no faster (2026-10-16 bench).
bool shadeSpanEnabled();
}  // namespace litholattice::swsimd