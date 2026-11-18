#pragma once
// Refactor step 6 (Direction JD, design Archie, implementation Cortana): cVkCanonUploader builds the Vulkan
// pulled layout (PulledMesh, packed_quad64.h) straight from a mesh's CanonicalMesh instead of re-encoding
// every decoded vertex with buildPulled. LL_VK_CANON=1 opts in (needs LL_VK_PULL=1); default off.
//  - Fallback quads (the per-cell prefix of ChunkMesh::verts) go through encodePulled exactly as buildPulled.
//  - Canonical quads use a per-thread template cache keyed by the quad's non-position bits and its palette
//    entry. A miss decodes one quad (decodeCanonicalMesh) and runs encodePulled once; hits only add origin.
//  - Same first-seen palette order, face buckets, blend split and 15-bit distance rule as buildPulled. Any
//    case it does not cover (no canonical, split mesh, size mismatch, maxQuads, distance) declines
//    and the caller uses buildPulled.
//  - Frame check: with mesh verify on (DebugOpt, or LL_CANON_VERIFY=1) the renderer also runs buildPulled
//    and compares the two layouts byte for byte (fallback as Packed12, palette, quads, quadMat, buckets).
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "litholattice/render/renderer.h"
#include "litholattice/render_vk/packed_quad64.h"

namespace litholattice::vk {

class cVkCanonUploader {
 public:
  /// LL_VK_CANON=1. Registers the exit stats line on first true call.
  static bool enabled();
  /// Builds `out` from m.canonical (L0). False = declined; out is then unspecified, call buildPulled.
  bool build(const ChunkMesh& m, uint32_t maxQuads, int blendMode, PulledMesh& out);
  /// Frame comparison: true when both layouts draw the same bytes.
  static bool sameLayout(const PulledMesh& a, const PulledMesh& b);
  /// Counts one frame check; describes the first few mismatches on stderr.
  static void noteCheck(const PulledMesh& canon, const PulledMesh& ref);
  static void printStats();

 private:
  struct Key {
    uint64_t quad, pal;
    bool operator==(const Key& o) const { return quad == o.quad && pal == o.pal; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      return size_t((k.quad * 0x9E3779B97F4A7C15ull) ^ (k.pal + 0x632BE59BD9B4E019ull + (k.quad << 6)));
    }
  };
  struct Tmpl {
    bool ok = false;      ///< decode gave one quad
    bool pulled = false;  ///< encodePulled accepted it
    uint64_t q = 0, e = 0;  ///< packed quad with origin bits cleared, material entry
    int delta[3] = {};      ///< pulled origin minus canonical origin (cells)
    PackedVertex local[4] = {};  ///< decoded verts minus origin*16
  };
  const Tmpl& lookup(uint64_t rawQuad, uint64_t palEntry);
  bool decline();

  std::unordered_map<Key, Tmpl, KeyHash> cache_;
  std::unordered_map<uint64_t, uint32_t> pal_;
  std::vector<uint64_t> qs_[6];
  std::vector<uint32_t> qm_[6];
  std::vector<uint32_t> idx_;
  std::vector<PackedVertex> one_;

  static inline std::atomic<uint64_t> built_{0}, declined_{0}, checked_{0}, mismatches_{0}, hits_{0}, misses_{0};
};

}  // namespace litholattice::vk