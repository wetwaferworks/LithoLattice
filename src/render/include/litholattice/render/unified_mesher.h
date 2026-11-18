// UnifiedMesher: one bitwise greedy mesher emitting a CanonicalMesh (Archie
// rulings Q21-Q28, Q35-Q39, Q42, Q50-Q53; migration step 3 core, 2026-10-29).
// 2026-11-17 (JD): LOD is gone. The mesher emits full geometry only; the L1/L2
// downsample and the perimeter skirts were removed.
//
// WHAT: reads an 18^3 apron (1..16 core) of material IDs + packed light, builds the six
//   face-visibility bitmasks with AND-NOT on 18-bit occupancy rows, and greedy-merges
//   each face slice with countr_zero/countr_one over 16-bit rows. Output is one
//   CanonicalMesh whose MeshHash is FNV-1a over its packed bytes.
// JD-approved fixes over the sketch:
//   (1) real AO: per exposed face an 8-bit neighbour-solid mask is built from shifted
//       occupancy rows (one pass per slice row), cornerAo comes from a 256-entry LUT, and
//       aoDiagFlip follows the AO (canon::aoDiagFlipFor).
//   (2) no palette pollution: merging compares raw material IDs; a palette slot is
//       taken only when a quad is emitted, in first-seen order. More than 1024 slots
//       returns Status::PaletteOverflow (logged once by the caller) - never a clamp.
//   (4) a match-key grid (raw material, AO, light, uvRot) is filled once per face slice
//       before merging, so the merge loop is integer compares only.
// Deterministic by construction: fixed face -> slice -> row -> bit order, no
// hashing containers, no thread-locals; the same input bytes give the same mesh bytes.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "litholattice/render/canonical_quad.h"

namespace litholattice {

struct UnifiedMaterial {
  canon::PaletteFields palette{};
  bool occludes = true;  ///< opaque full cube: hides neighbour faces and casts AO
  uint8_t uvRot = 0;
  /// M6 (registry feed): per-face looks. When set, face direction f (+X -X +Y -Y +Z -Z)
  /// takes faces[f] as its palette entry (one palette slot per used (material, face)).
  bool perFace = false;
  std::array<canon::PaletteFields, 6> faces{};
};

struct BucketRange { uint32_t start = 0, count = 0; };

/// Quad ranges of a canonical mesh (full geometry; the only level since LOD was removed).
struct CanonicalRanges {
  std::array<BucketRange, 6> face{};  ///< opaque/cutout/dither/OIT quads per face direction
  BucketRange blend{};                ///< alpha-blend quads, drawn back-to-front last
};

struct CanonicalMesh {
  uint16_t magic = canon::kMeshMagic, version = canon::kMeshVersion;
  CanonicalRanges ranges{};
  std::vector<uint64_t> quads;
  std::vector<uint64_t> palette;
  std::vector<canon::Packed12> fallback;  ///< non-cube geometry (filled by later steps)
  /// M11 (Archie ruling 1): palette overflow splits the sub-chunk into two meshes instead
  /// of falling back to the legacy stream. `next` is the second half (own palette of up to
  /// kMaxPalette entries). Stream order is preserved: decode(this) ++ decode(next) is the
  /// unsplit stream (this = fallback prefix [+ quad prefix], next = the rest). Null for
  /// every normal mesh.
  std::shared_ptr<CanonicalMesh> next;
  size_t residentBytes() const noexcept {
    return 64 + quads.size() * 8 + palette.size() * 8 + fallback.size() * 12 + (next ? next->residentBytes() : 0);
  }
};

/// FNV-1a 64 over header, ranges, quads, palette and fallback, little-endian words.
uint64_t canonicalMeshHash(const CanonicalMesh& m) noexcept;

/// 18^3 apron input, index (y*18+z)*18+x, core 1..16. matId 0 = air.
/// light: low 6 bits block light, bits 6..11 sky light (sampled from the air cell in
/// front of a face).
struct UnifiedMeshInput {
  const uint16_t* matId = nullptr;
  const uint16_t* light = nullptr;               ///< may be null (full sky, no block light)
  const UnifiedMaterial* materials = nullptr;    ///< indexed by matId
  uint32_t materialCount = 0;
  static constexpr int apronIdx(int x, int y, int z) noexcept { return (y * 18 + z) * 18 + x; }
};

class UnifiedMesher {
 public:
  enum class Status { Ok, PaletteOverflow, BadInput };
  UnifiedMesher();
  /// Builds the mesh into `out` (cleared first). On PaletteOverflow `out` is cleared and
  /// the caller must mesh this sub-chunk on the legacy path.
  Status mesh(const UnifiedMeshInput& in, CanonicalMesh& out);
  uint32_t lastQuadCount() const noexcept { return lastQuads_; }

 private:
  struct Grid;  // 18^3 mat/light apron grid
  void build(const Grid& g, CanonicalMesh& out, bool& overflow);
  int paletteSlot(uint16_t mat, uint32_t face, CanonicalMesh& out);
  const UnifiedMeshInput* in_ = nullptr;
  std::vector<uint32_t> slotStamp_;  // per (material ID, face slot): (stamp << 10) | slot
  uint32_t stamp_ = 0;
  uint32_t lastQuads_ = 0;
  std::vector<uint64_t> blendTmp_;
};

}  // namespace litholattice
