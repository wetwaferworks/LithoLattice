#pragma once
// 0.0.1.59 [P59-1] Packed12: the 12-byte GPU chunk vertex (bit-drop review layout "12Z").
// Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and
// the Bitmasker; layout correction and build by Cortana.
//
// Bytes 0..11 of PackedVertex are kept bit-for-bit, except z (bytes 4..5), which also carries the
// layer's high bits:
//   0-1 x | 2-3 y | 4-5 z[0..8] | layer[8..14] << 9 | 6 u | 7 v | 8 faceAo | 9 layer[0..7] | 10 value | 11 mode
// Decode: z = zl & 511, layer = layerLo | (zl >> 9) << 8. The 24-bit slot (no shader reads it) is dropped.
// Domain: z <= 511 (sub-chunk local 1/16 units; content uses 0..272) and layer <= 32767 (kMaxLayers is
// 2048). Values outside are clamped and counted: never a silent wrap. CPU-side only; the CPU
// PackedVertex, meshing and every sim hash are unchanged. Entities keep the 16-byte vertex.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "litholattice/render/renderer.h"

namespace litholattice::vk {

#pragma pack(push, 1)
struct GpuVertex12 {
  uint16_t x, y, zl;
  uint8_t u, v, faceAo, layerLo, value, mode;
};
#pragma pack(pop)
static_assert(sizeof(GpuVertex12) == 12, "Packed12 vertex layout");
static_assert(offsetof(PackedVertex, u) == 6 && offsetof(PackedVertex, mode) == 11, "PackedVertex prefix used by Packed12");

constexpr uint32_t kPacked12MaxZ = 511;
constexpr uint32_t kPacked12MaxLayer = 32767;

/// Packs one vertex. Returns false (and writes the clamped value) when z or the layer is outside
/// the Packed12 domain.
inline bool packVertex12(const PackedVertex& in, GpuVertex12& out) {
  GpuVertex12 g;
  std::memcpy(&g, &in, sizeof g);  // bytes 0..11 verbatim (x, y, z, u, v, faceAo, layer lo, value, mode)
  uint32_t z = in.z, layer = in.layerIndex();
  const bool ok = z <= kPacked12MaxZ && layer <= kPacked12MaxLayer;
  if (z > kPacked12MaxZ) z = kPacked12MaxZ;
  if (layer > kPacked12MaxLayer) layer = kPacked12MaxLayer;
  g.zl = uint16_t(z | ((layer >> 8) << 9));
  g.layerLo = uint8_t(layer & 0xFFu);
  out = g;
  return ok;
}

/// Inverse (what the shader decodes). The slot comes back as 0.
inline PackedVertex unpackVertex12(const GpuVertex12& g) {
  PackedVertex v{};
  v.x = g.x; v.y = g.y; v.z = uint16_t(g.zl & kPacked12MaxZ);
  v.u = g.u; v.v = g.v; v.faceAo = g.faceAo; v.value = g.value; v.mode = g.mode;
  v.setLayer(uint32_t(g.layerLo) | (uint32_t(g.zl >> 9) << 8));
  return v;
}

/// Packs `n` vertices into `dst` (n * 12 bytes, any alignment). Returns the number of clamped vertices.
inline uint32_t packVertices12(const PackedVertex* src, size_t n, void* dst) {
  uint32_t misses = 0;
  auto* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; ++i) {
    GpuVertex12 g;
    misses += packVertex12(src[i], g) ? 0u : 1u;
    std::memcpy(d + i * sizeof g, &g, sizeof g);
  }
  return misses;
}

}  // namespace litholattice::vk
