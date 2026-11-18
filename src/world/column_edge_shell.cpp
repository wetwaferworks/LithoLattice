#include "litholattice/world/column_edge_shell.h"

namespace litholattice {

namespace {
struct RingLut {
  std::array<int8_t, 256> v{};
  RingLut() {
    int n = 0;
    for (int lz = 0; lz < 16; ++lz)
      for (int lx = 0; lx < 16; ++lx)
        v[size_t(lz * 16 + lx)] = int8_t((lx == 0 || lx == 15 || lz == 0 || lz == 15) ? n++ : -1);
  }
};
const RingLut& ringLut() { static const RingLut l; return l; }
}  // namespace

int cColumnEdgeShell::ringIndex(int lx, int lz) { return ringLut().v[size_t((lz & 15) * 16 + (lx & 15))]; }

std::shared_ptr<const cColumnEdgeShell> cColumnEdgeShell::build(const ChunkColumn& c) {
  auto sh = std::make_shared<cColumnEdgeShell>();
  sh->bytes_ = sizeof(cColumnEdgeShell);
  for (int sy = 0; sy < kSubChunks; ++sy) {
    const SubChunk* s = c.subs[size_t(sy)].get();
    if (!s) continue;
    auto L = std::make_unique<Layer>();
    bool any = false;
    for (int y = 0; y < 16; ++y)
      for (int lz = 0; lz < 16; ++lz)
        for (int lx = 0; lx < 16; ++lx) {
          const int r = ringIndex(lx, lz);
          if (r < 0) continue;
          const int i = y * kRingPerLayer + r;
          const StableId sid = s->at(lx, y, lz);
          const CellState st = s->state[size_t(SubChunk::index(lx, y, lz))];
          const uint8_t m = s->materialAt(lx, y, lz);
          if (!isAirStableId(sid) || st || m) any = true;
          size_t p = 0;
          while (p < L->palette.size() && L->palette[p] != sid) ++p;
          if (p == L->palette.size()) L->palette.push_back(sid);
          L->idx[size_t(i)] = uint16_t(p);
          L->state[size_t(i)] = st;
          L->mat[size_t(i)] = m;
        }
    if (!any) continue;  // ring all air: the sub reads air
    sh->bytes_ += sizeof(Layer) + L->palette.capacity() * sizeof(StableId);
    sh->subs_[size_t(sy)] = std::move(L);
  }
  return sh;
}

StableId cColumnEdgeShell::at(int sy, int lx, int y, int lz) const {
  if (!hasSub(sy) || ringIndex(lx, lz) < 0) return kAirStableId;
  const Layer& L = *subs_[size_t(sy)];
  return L.palette[L.idx[size_t(slot(lx, y, lz))]];
}

CellState cColumnEdgeShell::stateAt(int sy, int lx, int y, int lz) const {
  if (!hasSub(sy) || ringIndex(lx, lz) < 0) return 0;
  return subs_[size_t(sy)]->state[size_t(slot(lx, y, lz))];
}

void cColumnEdgeShell::expand(int sy, SubChunk& out) const {
  if (!hasSub(sy)) return;
  const Layer& L = *subs_[size_t(sy)];
  for (int y = 0; y < 16; ++y)
    for (int lz = 0; lz < 16; ++lz)
      for (int lx = 0; lx < 16; ++lx) {
        const int r = ringIndex(lx, lz);
        if (r < 0) continue;
        const size_t i = size_t(y * kRingPerLayer + r);
        const StableId sid = L.palette[L.idx[i]];
        if (!isAirStableId(sid)) out.set(lx, y, lz, sid);
        out.state[size_t(SubChunk::index(lx, y, lz))] = L.state[i];
        if (L.mat[i]) out.setMaterial(lx, y, lz, L.mat[i]);
      }
}

}  // namespace litholattice
