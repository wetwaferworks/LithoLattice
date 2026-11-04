// IUnifiedMesher refactor step 3b (UML_UNIFIED_MESHER_2026-10-30.md): the software backend's
// canonical ingest store (M8, LL_SW_CANON) behind IMeshUploader. Wrapping only: the
// validation and staging moved verbatim out of cSoftwareRenderer::uploadChunkMesh. Decoding,
// LOD selection and seams stay in the renderer. A placement's `first` carries the store key.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>

#include "litholattice/render/IMeshUploader.h"
#include "litholattice/render/renderer.h"

namespace litholattice {

class cSwCanonStore final : public IMeshUploader {
 public:
  /// One stored canonical mesh: per-chunk params (no legacy stream), the drawn lod
  /// (-1 = none), the lod being selected this pass, and the decoded skirt mask (M10).
  struct Entry { ChunkMesh meta; int lod = -1; int pend = 0; uint8_t skirtMask = 0xFF; };

  /// Same key as the software renderer's mesh store.
  static int64_t keyOf(int cx, int sy, int cz) {
    return int64_t((uint64_t(uint32_t(cx)) << 40) ^ (uint64_t(uint32_t(sy)) << 32) ^ uint64_t(uint32_t(cz)));
  }

  /// False when the mesh carries no decodable canonical payload (empty, none, bad magic).
  bool prepare(const ChunkMesh& m) override;
  /// Stores the staged entry (replacing any old one); out.first = key, out.count = 1.
  bool commit(MeshPlacement& out) override;
  void release(const MeshPlacement& p) override;
  void clear() override { entries_.clear(); }
  size_t residentUnits() const override { return entries_.size(); }

  std::map<int64_t, Entry>& entries() { return entries_; }
  const std::map<int64_t, Entry>& entries() const { return entries_; }

 private:
  std::map<int64_t, Entry> entries_;
  Entry staged_;
  int64_t stagedKey_ = 0;
};

}  // namespace litholattice
