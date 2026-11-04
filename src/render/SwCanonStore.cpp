#include "litholattice/render/SwCanonStore.h"

#include <utility>

#include "litholattice/render/canonical_quad.h"
#include "litholattice/render/unified_mesher.h"

namespace litholattice {

bool cSwCanonStore::prepare(const ChunkMesh& m) {
  if (m.verts.empty() || !m.canonical || m.canonical->magic != canon::kMeshMagic) return false;
  Entry e;
  e.meta.cx = m.cx; e.meta.sy = m.sy; e.meta.cz = m.cz;
  {
    ChunkMesh tmp = m;  // copy the per-chunk params, drop the legacy stream
    tmp.verts = {};
    e.meta = std::move(tmp);
  }
  staged_ = std::move(e);
  stagedKey_ = keyOf(m.cx, m.sy, m.cz);
  return true;
}

bool cSwCanonStore::commit(MeshPlacement& out) {
  entries_[stagedKey_] = std::move(staged_);
  staged_ = Entry{};
  out = MeshPlacement{};
  out.first = size_t(uint64_t(stagedKey_));
  out.count = 1;
  return true;
}

void cSwCanonStore::release(const MeshPlacement& p) { entries_.erase(int64_t(uint64_t(p.first))); }

}  // namespace litholattice
