// IUnifiedMesher refactor step 3 (UML_UNIFIED_MESHER_2026-10-30.md): a backend's canonical
// mesh uploader. Two-phase so a backend keeps its exact ordering: prepare() stages and
// validates (false = keep the legacy path), the backend drops its old entry, then commit()
// places the staged mesh (false = no room, legacy path). Wrapping only: no format change.
#pragma once
#include <cstddef>
#include <cstdint>

#include "litholattice/render/renderer.h"

namespace litholattice {

/// Where a committed canonical mesh lives (backend-defined units, e.g. TBO texels).
struct MeshPlacement {
  uint32_t slab = 0;
  size_t first = 0, count = 0;
  uint32_t fallbackQuads = 0, quads = 0;
};

class IMeshUploader {
 public:
  virtual ~IMeshUploader() = default;
  virtual bool prepare(const ChunkMesh& m) = 0;
  virtual bool commit(MeshPlacement& out) = 0;
  virtual void release(const MeshPlacement& p) = 0;
  virtual void clear() = 0;
  virtual size_t residentUnits() const = 0;
};

}  // namespace litholattice
