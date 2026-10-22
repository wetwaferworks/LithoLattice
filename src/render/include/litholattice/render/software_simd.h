// Cortana 2026-10-15 1455: software-renderer SIMD runtime dispatch.
// Baseline (SSE2 / NEON / scalar) code is always built; the AVX2 kernels
// live in software_simd_avx2.cpp with per-function target attributes and
// are chosen at first use only when CPUID + XGETBV report AVX2 with OS YMM
// support. LL_SW_FORCE_SCALAR=1 forces the baseline path (identity proofs).
// Every kernel has bit-identical output on both paths (pure stores).
#pragma once
#include <cstddef>
#include <cstdint>
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
  float* depth;                         ///< depth row at x0
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