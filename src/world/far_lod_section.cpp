#include "litholattice/world/far_lod_section.h"
#include "litholattice/world/block.h"

namespace litholattice {

namespace {
struct Pick {  // rank: 2 opaque, 1 other non-air, 0 air
  uint16_t id = 0;
  int rank = 0;
};
}  // namespace

std::unique_ptr<cFarLodSection> cFarLodSection::build(const ChunkColumn& c, const cBlockReg* reg) {
  auto out = std::make_unique<cFarLodSection>();
  out->cx_ = c.cx;
  out->cz_ = c.cz;
  std::vector<int> rankOf{0};  // per palette entry
  auto intern = [&](StableId sid) -> uint16_t {
    if (isAirStableId(sid)) return 0;
    for (size_t i = 1; i < out->palette_.size(); ++i)
      if (out->palette_[i] == sid) return uint16_t(i);
    if (out->palette_.size() >= 0xffff) return 0;
    int r = 1;
    if (reg) { const auto id = reg->findByStableId(sid); if (id && reg->has(*id, kOpaque)) r = 2; }
    else r = 2;
    out->palette_.push_back(sid);
    rankOf.push_back(r);
    return uint16_t(out->palette_.size() - 1);
  };
  std::array<uint16_t, kSubChunkVolume> l0{};
  for (int sy = 0; sy < kSubChunks; ++sy) {
    const SubChunk* s = c.subs[size_t(sy)].get();
    if (!s || !s->nonAir) continue;
    for (int i = 0; i < kSubChunkVolume; ++i) l0[size_t(i)] = intern(s->blocks.at(size_t(i)));
    auto cells = std::make_unique<std::array<Cell, kCellsPerSub>>();
    for (int lvl = 1; lvl <= kLevels; ++lvl) {
      const int d = dim(lvl), cd = d * 2;
      for (int z = 0; z < d; ++z)
        for (int y = 0; y < d; ++y)
          for (int x = 0; x < d; ++x) {
            uint16_t ids[8];
            uint8_t light = 0, mask = 0;
            for (int k = 0; k < 8; ++k) {
              const int ix = x * 2 + (k & 1), iy = y * 2 + (k >> 1 & 1), iz = z * 2 + (k >> 2 & 1);
              if (lvl == 1) {
                ids[k] = l0[size_t(SubChunk::index(ix, iy, iz))];
              } else {
                const Cell& ch = (*cells)[size_t(offset(lvl - 1) + (iz * cd + iy) * cd + ix)];
                ids[k] = ch.id;
                if (ch.light > light) light = ch.light;
              }
              if (ids[k]) mask = uint8_t(mask | (1u << k));
            }
            Pick best;
            int bestCount = 0;
            for (int k = 0; k < 8; ++k) {
              const int r = rankOf[ids[k]];
              int n = 0;
              for (int j = 0; j < 8; ++j) n += ids[j] == ids[k];
              if (r > best.rank || (r == best.rank && n > bestCount)) { best = {ids[k], r}; bestCount = n; }
            }
            Cell& dst = (*cells)[size_t(offset(lvl) + (z * d + y) * d + x)];
            dst.id = best.id;
            dst.childMask = mask;
            dst.light = light;
          }
    }
    out->subs_[size_t(sy)] = std::move(cells);
  }
  return out;
}

const cFarLodSection::Cell* cFarLodSection::find(uint64_t k) const {
  const int lvl = int(k >> 60);
  if (lvl < 1 || lvl > kLevels) return nullptr;
  auto sx = [](uint64_t v, int bits) { const int64_t m = int64_t(1) << (bits - 1); return int(int64_t(v ^ uint64_t(m)) - m); };
  const int x = sx(k & 0xffffff, 24), z = sx(k >> 24 & 0xffffff, 24), y = int(k >> 48 & 0xfff);
  const int d = dim(lvl);
  const int bx = x - ((cx_ * 16) >> lvl), bz = z - ((cz_ * 16) >> lvl);
  if (bx < 0 || bx >= d || bz < 0 || bz >= d) return nullptr;
  const int sy = y / d, ly = y % d;
  if (sy < 0 || sy >= kSubChunks || !subs_[size_t(sy)]) return nullptr;
  return &(*subs_[size_t(sy)])[size_t(offset(lvl) + (bz * d + ly) * d + bx)];
}

size_t cFarLodSection::bytes() const {
  size_t b = sizeof(*this) + palette_.capacity() * sizeof(StableId);
  for (const auto& s : subs_) if (s) b += sizeof(*s);
  return b;
}

}  // namespace litholattice
