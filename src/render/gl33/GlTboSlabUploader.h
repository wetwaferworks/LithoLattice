// Private to the gl33 backend: the M12 canonical pull-stream uploader (LL_GL_CANON) behind
// IMeshUploader (refactor step 3). Owns the fix (c) slab pool. Logic moved verbatim out of
// cGL33Renderer::uploadCanon. GL thread only.
#pragma once
#include <cstddef>
#include <cstdint>

#include "GlTboSlabPool.h"
#include "litholattice/render/IMeshUploader.h"
#include "litholattice/render/mesher.h"

namespace litholattice::gl33 {

class cGlTboSlabUploader final : public IMeshUploader {
 public:
  bool prepare(const ChunkMesh& m) override;
  bool commit(MeshPlacement& out) override;
  void release(const MeshPlacement& p) override;
  void clear() override;
  size_t residentUnits() const override { return texels_; }

  cGlTboSlabPool& pool() { return pool_; }
  uint64_t meshes() const { return meshes_; }
  uint64_t rejects() const { return rejects_; }

 private:
  cGlTboSlabPool pool_;  // fix (c): paged 64 MiB slabs
  CanonPullStream ps_;   // staged by prepare(), consumed by commit()
  size_t texels_ = 0;
  uint64_t meshes_ = 0, rejects_ = 0;
};

}  // namespace litholattice::gl33
