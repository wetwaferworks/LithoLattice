#pragma once
// Fix (a), Archie 2026-10-30: per-reason Packed12 fallback counters for the unified mesher.
// One class (JD rule: no loose free functions). Mesh jobs accumulate into a job-local
// Local, then flush() once per sub-chunk with relaxed fetch_add (no per-face atomics).
#include <atomic>
#include <cstdint>
#include <string>

namespace litholattice {

enum class FallbackReason : uint8_t {
  NonUnitSubBlock = 0,  ///< slab faces (half-height box) and other partial boxes
  FaceRotation,         ///< Look::rot != 0 (directional top/bottom texture turn)
  UVOverride,           ///< Look::uv != default (sub-tile UV rect)
  TintOrPulse,          ///< trace / strip tint modes, or a strength-buffer slot
  CustomNormalOrShape,  ///< non-face-loop geometry: crosses, box models, shaped / custom blocks, extras
  PaletteOrLookLimit,   ///< palette / look over the canonical width (not a per-face gate today)
  AoCorners,            ///< non-uniform corner AO (the canonical quad carries one AO value)
  ShadeMode,            ///< any other non-greedy shade mode (cutout, leaf, glass, animated, ...)
  Count
};

/// Breakdown of CustomNormalOrShape: which non-face-loop branch wrote the Packed12 quads.
enum class FallbackShape : uint8_t { BoxModel = 0, Shaped, Custom, Cross, Extra, Count };

class FallbackReasonStats {
 public:
  static constexpr int kCount = int(FallbackReason::Count);
  struct Local {  ///< job-local accumulator (one per sub-chunk mesh job)
    uint32_t reasons[kCount]{};
    uint32_t modes[64]{};       ///< ShadeMode histogram of per-face fallback faces
    uint32_t aoPatterns[256]{};  ///< corner AO pattern histogram for AoCorners faces
    uint32_t faceFallback = 0;   ///< per-face fallback quads (sum of the face reasons)
    uint32_t canonFaces = 0;     ///< unit faces parked for the canonical flush
    uint32_t shapes[int(FallbackShape::Count)]{};  ///< attributed CustomNormalOrShape quads
    void add(FallbackReason r, uint32_t quads, uint8_t mode, const int ao[4]);
    uint32_t customBlocks[256]{};  ///< Shape::Custom quads by block id (ids >= 255 share the last slot)
    void addShape(FallbackShape s, size_t quads) { shapes[int(s)] += uint32_t(quads); }
    void addCustom(uint32_t blockId, size_t quads) {
      addShape(FallbackShape::Custom, quads);
      customBlocks[blockId < 255 ? blockId : 255] += uint32_t(quads);
    }
  };
  /// First failing condition of the greedy / canonical eligibility gate (Count = eligible).
  /// Same conditions and result as !slab && greedyEligible(lk, ao).
  static FallbackReason checkCanonicalEligibility(bool slab, uint8_t mode, uint8_t rot, uint32_t slot, bool uvDefault,
                                                  const int ao[4]) noexcept;
  static FallbackReasonStats& instance();
  /// totalFallbackQuads = Packed12 quads in the mesh (legacy stream / 4); the remainder
  /// after the per-face reasons is non-face-loop geometry (CustomNormalOrShape).
  void flush(Local& l, uint64_t totalFallbackQuads, uint64_t canonicalQuads);
  std::string reasonsLine() const;  ///< "[unified-mesher-reasons] ..." (no newline)
  std::string detailLine() const;   ///< top bucket's concrete condition

 private:
  std::atomic<uint64_t> counts_[kCount]{};
  std::atomic<uint64_t> modes_[64]{};
  std::atomic<uint64_t> ao_[256]{};
  std::atomic<uint64_t> canonicalQuads_{0}, canonFaces_{0};
  std::atomic<uint64_t> shapes_[int(FallbackShape::Count)]{};
  std::atomic<uint64_t> custom_[256]{};
};

}  // namespace litholattice
