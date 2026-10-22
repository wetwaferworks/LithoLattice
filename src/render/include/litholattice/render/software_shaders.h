// Cortana 2026-10-15 0452: PORT LL_SQRTF
// Software shaders: basic per-pixel lighting for the CPU renderer.
// Vertex shader: transform + pass varyings (normal, UV, world pos).
// Fragment shader: texture sample + diffuse lighting (N.L) + ambient.
// Enough for Chromebooks and GPU-less machines to run the game.
#pragma once
#include <cstdint>
#if defined(_MSC_VER) && !defined(__clang__)
#include <cmath>
#define LL_SQRTF(x) std::sqrt(static_cast<float>(x))  // MSVC: no __builtin_sqrtf; IEEE sqrt is correctly rounded either way
#else
#define LL_SQRTF(x) __builtin_sqrtf(x)
#endif

namespace litholattice {

/// PBR per-pixel context (defined in pbr_software_shaders.h); FragmentInput only holds
/// a pointer so this header stays dependency-free.
struct PbrFragmentContext;

/// Varyings interpolated across a triangle (per-pixel inputs).
struct ShaderVaryings {
  float nx, ny, nz;  ///< normal (interpolated)
  float u, v;        ///< texture UV
  uint32_t layer;    ///< texture layer
  float wx, wy, wz;  ///< world position (for future point lights)
  float ao = 1.0f;   ///< Cortana 2026-10-16 (render.ao): baked corner AO factor (1 = open)
  uint16_t materialId = 0xFFFF;  ///< compact MaterialRegistry ID (flat per quad, like
                                 ///  layer); 0xFFFF = none -> basic shading
};

/// Fragment shader input/output.
struct FragmentInput {
  ShaderVaryings varyings;
  const uint32_t* texData;  ///< texture array (color)
  uint32_t texLayers;
  const uint32_t* normalData;  ///< normal map array (optional, RGB = XYZ normal)
  uint32_t normalLayers;
  const uint8_t* heightData;  ///< heightmap array (optional, 8-bit height per texel)
  uint32_t heightLayers;
  float bumpScale = 1.0f;     ///< bump intensity
  const PbrFragmentContext* pbr = nullptr;  ///< PBR context for pbrFragmentShader
                                            ///  (pbr_software_shaders.h); null = basic
  bool texMorton = false;  ///< Pass 2 step 4: texData layers are Morton-swizzled
  // Cortana 2026-10-16 (BUG-0003): runtime colour tile size S (16/32/64): shift = log2 S,
  // mask = S-1, area = S*S, scale = S-1 (the historic u*15 mapping, generalised). Defaults = 16.
  uint32_t texShift = 4, texMask = 15, texArea = 256;
  float texScale = 15.0f;
};

// Ginny (Argon) 2026-10-16 Pass 2 step 4: Morton (Z-order) texel addressing for the
// 16x16 software tiles. MortonU spreads tx's 4 bits to the even bit positions, MortonV
// spreads ty's to the odd ones; index = MortonU[tx] | MortonV[ty].
inline constexpr uint8_t kSwMortonU[16] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
                                           0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55};
inline constexpr uint8_t kSwMortonV[16] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A,
                                           0x80, 0x82, 0x88, 0x8A, 0xA0, 0xA2, 0xA8, 0xAA};
/// Texel index inside one 256-texel layer for the input's layout.
inline uint32_t swTexelIndex(const FragmentInput& in, uint32_t tx, uint32_t ty) {
  return in.texMorton ? uint32_t(kSwMortonU[tx] | kSwMortonV[ty]) : (ty << in.texShift) + tx;
}

// Cortana 2026-10-15 1705: greedy meshing [ENGINE]
/// Greedy quads carry u/v up to 15 tiles. Values in [0,1] pass through bit-exact
/// (so per-block quads sample exactly as before); larger values wrap per tile.
inline float swTile(float t) { return t > 1.0f ? t - float(int(t)) : t; }

/// Sample a normal map and decode to XYZ (RGB 0-255 -> -1.0 to 1.0).
inline void sampleNormalMap(const FragmentInput& in, float u, float v, uint32_t layer,
                            float* nx, float* ny, float* nz) {
  if (!in.normalData || layer >= in.normalLayers) {
    *nx = in.varyings.nx; *ny = in.varyings.ny; *nz = in.varyings.nz;
    return;
  }
  uint32_t tx = uint32_t(swTile(u) * 15.0f) & 15;  // Cortana 2026-10-15 1705: swTile
  uint32_t ty = uint32_t(swTile(v) * 15.0f) & 15;
  uint32_t n = in.normalData[layer * 256 + ty * 16 + tx];
  *nx = ((n >> 16) & 0xFF) / 127.5f - 1.0f;
  *ny = ((n >> 8) & 0xFF) / 127.5f - 1.0f;
  *nz = (n & 0xFF) / 127.5f - 1.0f;
}

/// Bump mapping: sample heightmap, compute gradient, perturb the normal.
/// Uses central differences on the 8-bit heightfield.
inline void applyBumpMap(const FragmentInput& in, float u, float v, uint32_t layer,
                         float* nx, float* ny, float* nz) {
  if (!in.heightData || layer >= in.heightLayers) return;
  auto heightAt = [&](int tx, int ty) -> float {
    tx = (tx < 0) ? 0 : (tx > 15 ? 15 : tx);
    ty = (ty < 0) ? 0 : (ty > 15 ? 15 : ty);
    return in.heightData[layer * 256 + ty * 16 + tx] / 255.0f;
  };
  int tx = int(swTile(u) * 15.0f), ty = int(swTile(v) * 15.0f);  // Cortana 2026-10-15 1705: swTile
  float hL = heightAt(tx-1, ty), hR = heightAt(tx+1, ty);
  float hD = heightAt(tx, ty-1), hU = heightAt(tx, ty+1);
  float dx = (hR - hL) * in.bumpScale;
  float dy = (hU - hD) * in.bumpScale;
  // Perturb: assume tangent space aligns with UV
  *nx -= dx; *ny -= dy;
  // Renormalize
  float len = *nx**nx + *ny**ny + *nz**nz;
  if (len > 1e-6f) { len = 1.0f / LL_SQRTF(len); *nx*=len; *ny*=len; *nz*=len; }  // EXACT (bug-fix run): feeds the pinned pbr-software equality path
}

/// Basic shader: diffuse + ambient + texture.
/// Light dir is fixed (top-down-ish). Returns RGBA.
inline uint32_t basicFragmentShader(const FragmentInput& in) {
  // Texture sample (nearest neighbor for speed; bilinear is future)
  uint32_t texColor = 0xFFFFFFFF;
  if (in.texData && in.varyings.layer < in.texLayers) {
    uint32_t tx = uint32_t(swTile(in.varyings.u) * in.texScale) & in.texMask;  // Cortana 2026-10-16: runtime tile
    uint32_t ty = uint32_t(swTile(in.varyings.v) * in.texScale) & in.texMask;
    texColor = in.texData[in.varyings.layer * in.texArea + swTexelIndex(in, tx, ty)];
  }

  // Normal mapping: sample normal map if available, else use interpolated normal
  float nx, ny, nz;
  sampleNormalMap(in, in.varyings.u, in.varyings.v, in.varyings.layer, &nx, &ny, &nz);
  // Bump mapping: perturb with heightfield gradient (if heightmap present)
  applyBumpMap(in, in.varyings.u, in.varyings.v, in.varyings.layer, &nx, &ny, &nz);

  // Diffuse: N.L with fixed light direction (normalized ~ (0.5, 1.0, 0.3))
  float nl = nx * 0.44f + ny * 0.88f + nz * 0.26f;
  if (nl < 0) nl = 0;

  // Ambient + diffuse
  float light = 0.35f + 0.65f * nl;  // 0.35 ambient, 0.65 diffuse

  uint8_t tr = (texColor >> 16) & 0xFF;
  uint8_t tg = (texColor >> 8) & 0xFF;
  uint8_t tb = texColor & 0xFF;
  uint8_t r = uint8_t(tr * light);
  uint8_t g = uint8_t(tg * light);
  uint8_t b = uint8_t(tb * light);
  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/// Shader type: function pointer for the fragment stage.
/// (Vertex stage is fixed: transform + interpolate varyings.)
using FragmentShaderFn = uint32_t (*)(const FragmentInput&);

}  // namespace litholattice
