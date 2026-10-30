#include "litholattice/world/column_table.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/mesh_input.h"

namespace litholattice {

namespace {
std::atomic<int> g_cow{-1};
std::atomic<uint64_t> g_staleNoted{0}, g_staleFlushed{0}, g_stalePinned{0}, g_maxProbe{0};
std::atomic<uint64_t> g_pins{0}, g_subs{0}, g_full{0}, g_evict{0}, g_vChecked{0}, g_vMismatch{0}, g_bytes{0}, g_gathers{0};
size_t budgetBytes() {
  static const size_t b = [] {
    const char* e = std::getenv("LL_COW_BUDGET_MB");
    const long mb = e ? std::atol(e) : 128;
    return size_t(mb > 0 ? mb : 128) << 20;
  }();
  return b;
}
size_t snapBytes(const ColumnSnap& s) {
  size_t n = sizeof(ColumnSnap);
  for (const auto& p : s.subs) if (p) n += p->bytes();
  return n;
}
}  // namespace

size_t SubSnap::bytes() const {
  // state[] dominates; the paletted block/material storage is small next to it.
  return sizeof(SubSnap) + lut.size() * sizeof(BlockId) + full.size() * sizeof(BlockId) + 1024;
}

bool cowColumnsEnabled() {
  int v = g_cow.load(std::memory_order_relaxed);
  if (v < 0) {
    const char* e = std::getenv("LL_LEGACY_SNAPSHOT");
    int expect = -1;
    g_cow.compare_exchange_strong(expect, (e && e[0] == '1') ? 0 : 1);
    v = g_cow.load(std::memory_order_relaxed);
  }
  return v == 1;
}
void setCowColumns(bool on) { g_cow.store(on ? 1 : 0, std::memory_order_relaxed); }
CowStats cowStats() {
  CowStats s;
  s.pins = g_pins.load(); s.subsPublished = g_subs.load(); s.columnsFullPublished = g_full.load();
  s.evictedColumns = g_evict.load(); s.verifyChecked = g_vChecked.load(); s.verifyMismatches = g_vMismatch.load();
  s.retainedBytes = g_bytes.load(); s.gathers = g_gathers.load();
  s.staleNoted = g_staleNoted.load(); s.staleFlushed = g_staleFlushed.load(); s.stalePinned = g_stalePinned.load(); s.maxProbe = g_maxProbe.load();
  return s;
}
void noteCowVerify(bool match) {
  g_vChecked.fetch_add(1, std::memory_order_relaxed);
  if (!match) g_vMismatch.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const ColumnSnap> ColumnTable::publishLocked(const ChunkMap& w, const cBlockReg& reg, int cx, int cz,
                                                            uint64_t mask) {
  const ChunkColumn* c = w.column(cx, cz);
  const uint64_t key = ChunkKey::make(cx, cz);
  if (!c) {
    if (Entry* it = entries_.find(key)) { bytes_ -= it->bytes; entries_.erase(key); }
    stale_.erase(key);
    return nullptr;
  }
  Entry* it = entries_.find(key);
  auto snap = std::make_shared<ColumnSnap>();
  const bool whole = !it || !it->snap || it->snap->instanceId != c->instanceId;
  if (whole) {
    mask = ~uint64_t(0);
    g_full.fetch_add(1, std::memory_order_relaxed);
  } else {
    snap->subs = it->snap->subs;  // CoW: untouched subs are shared
  }
  snap->instanceId = c->instanceId;
  snap->commitEpoch = ++epoch_;
  snap->matPalette = c->materialPalette;
  snap->cellText.assign(c->cellText.begin(), c->cellText.end());
  for (int sy = 0; sy < kSubChunks; ++sy) {
    if (!(mask >> sy & 1)) continue;
    const SubChunk* s = c->subs[size_t(sy)].get();
    if (!s) { snap->subs[size_t(sy)] = nullptr; continue; }
    auto ss = std::make_shared<SubSnap>();
    ss->data.blocks = s->blocks;
    ss->data.state = s->state;
    ss->data.materials = s->materials;
    ss->data.nonAir = s->nonAir;
    if (s->blocks.width() == VoxelPalette::Width::W64) {
      ss->full.resize(kSubChunkVolume);
      for (int i = 0; i < kSubChunkVolume; ++i) {
        const auto id = reg.findByStableId(s->blocks.at(size_t(i)));
        ss->full[size_t(i)] = id ? *id : kAir;
      }
    } else {
      s->blocks.resolveBlockIds(reg, ss->lut);
    }
    snap->subs[size_t(sy)] = std::move(ss);
    g_subs.fetch_add(1, std::memory_order_relaxed);
  }
  if (uint64_t* st = stale_.find(key)) {  // published now: those stale bits are clean
    *st &= ~mask;
    if (!*st) stale_.erase(key);
  }
  Entry& e = entries_[key];
  bytes_ -= e.bytes;
  e.snap = snap;
  e.bytes = snapBytes(*snap);
  e.lastUse = ++use_;
  bytes_ += e.bytes;
  return snap;
}

void ColumnTable::enforceBudgetLocked() {
  const size_t budget = budgetBytes();
  if (bytes_ <= budget) { g_bytes.store(bytes_, std::memory_order_relaxed); return; }
  std::vector<std::pair<uint64_t, uint64_t>> order;  // (lastUse, key)
  order.reserve(entries_.size());
  entries_.forEach([&](uint64_t k, const Entry& e) { order.emplace_back(e.lastUse, k); });
  std::sort(order.begin(), order.end());
  const size_t target = budget - budget / 8;
  for (const auto& [use, k] : order) {
    if (bytes_ <= target) break;
    bytes_ -= entries_.find(k)->bytes;
    entries_.erase(k);
    g_evict.fetch_add(1, std::memory_order_relaxed);
  }
  g_bytes.store(bytes_, std::memory_order_relaxed);
}

void ColumnTable::noteStale(int cx, int sy, int cz) {
  if (sy < 0 || sy >= kSubChunks) return;
  std::lock_guard l(m_);
  stale_[ChunkKey::make(cx, cz)] |= uint64_t(1) << sy;
  g_staleNoted.fetch_add(1, std::memory_order_relaxed);
}

std::vector<std::pair<uint64_t, uint64_t>> ColumnTable::takeStale() {
  std::lock_guard l(m_);
  std::vector<std::pair<uint64_t, uint64_t>> out;
  out.reserve(stale_.size());
  stale_.forEach([&](uint64_t k, uint64_t m) { if (m) out.emplace_back(k, m); });
  stale_.clear();
  std::sort(out.begin(), out.end());
  return out;
}

void ColumnTable::publish(const ChunkMap& w, const cBlockReg& reg, int cx, int cz, uint64_t mask) {
  std::lock_guard l(m_);
  publishLocked(w, reg, cx, cz, mask);
  enforceBudgetLocked();
}

std::shared_ptr<const MeshPin> ColumnTable::pin(const ChunkMap& w, const cBlockReg& reg, int cx, int sy, int cz,
                                                BlockId belowWorld, MeshEdge edge) {
  auto p = std::make_shared<MeshPin>();
  p->cx = cx; p->sy = sy; p->cz = cz; p->belowWorld = belowWorld; p->edge = edge;
  std::lock_guard l(m_);
  for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx) {
      const size_t i = size_t((dz + 1) * 3 + (dx + 1));
      const ChunkColumn* c = w.column(cx + dx, cz + dz);
      p->present[i] = c != nullptr;
      if (!c) continue;
      const uint64_t key = ChunkKey::make(cx + dx, cz + dz);
      Entry* it = entries_.find(key);
      if (!it || !it->snap || it->snap->instanceId != c->instanceId) {
        p->cols[i] = publishLocked(w, reg, cx + dx, cz + dz, ~uint64_t(0));
      } else if (const uint64_t* st = stale_.find(key); st && *st) {
        g_stalePinned.fetch_add(1, std::memory_order_relaxed);
        p->cols[i] = publishLocked(w, reg, cx + dx, cz + dz, *st);  // stale state: republish first
      } else {
        it->lastUse = ++use_;
        p->cols[i] = it->snap;
      }
    }
  p->commitEpoch = epoch_;
  if (entries_.maxProbe() > g_maxProbe.load(std::memory_order_relaxed) && (g_pins.load(std::memory_order_relaxed) & 1023) == 0)
    g_maxProbe.store(entries_.maxProbe(), std::memory_order_relaxed);
  enforceBudgetLocked();
  g_pins.fetch_add(1, std::memory_order_relaxed);
  return p;
}

void ColumnTable::drop(int cx, int cz) {
  std::lock_guard l(m_);
  const uint64_t key = ChunkKey::make(cx, cz);
  if (Entry* it = entries_.find(key)) {
    bytes_ -= it->bytes;
    entries_.erase(key);
  }
  stale_.erase(key);
}

size_t ColumnTable::columns() const {
  std::lock_guard l(m_);
  return entries_.size();
}

// ---- worker side ----
void gatherPinned(MeshInput& out) {
  if (!out.pin) return;
  const std::shared_ptr<const MeshPin> pin = std::move(out.pin);
  out.pin.reset();
  g_gathers.fetch_add(1, std::memory_order_relaxed);
  const int sy = pin->sy;
  const ColumnSnap* own = pin->cols[4].get();
  out.matPaletteSize = 0;
  if (own) {
    out.matPaletteSize = own->matPalette.size();
    for (size_t i = 0; i < own->matPalette.size() && i < 256; ++i) out.matPalette[i] = own->matPalette[i];
  }
  out.text.clear();
  if (own)
    for (const auto& [k, t] : own->cellText) {
      const int y = int(k >> 8);
      if ((y >> 4) == sy) out.text.emplace_back(uint16_t(SubChunk::index(int(k & 15), y & 15, int((k >> 4) & 15))), t);
    }
  for (int dy = -1; dy <= 1; ++dy)
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        const size_t ci = size_t((dz + 1) * 3 + (dx + 1));
        const bool clamp = pin->edge == MeshEdge::Clamp && (dx || dz) && !pin->present[ci];
        const ColumnSnap* col = clamp ? own : pin->cols[ci].get();
        const int ssy = sy + dy;
        const SubSnap* s = (col && ssy >= 0 && ssy < kSubChunks) ? col->subs[size_t(ssy)].get() : nullptr;
        const bool under = ssy < 0;
        const bool w64 = s && !s->full.empty();
        const int x0 = dx < 0 ? 15 : 0, x1 = dx > 0 ? 0 : 15;
        const int y0 = dy < 0 ? 15 : 0, y1 = dy > 0 ? 0 : 15;
        const int z0 = dz < 0 ? 15 : 0, z1 = dz > 0 ? 0 : 15;
        for (int y = y0; y <= y1; ++y)
          for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
              const int i = meshPadIndex(x + 1 + dx * 16, y + 1 + dy * 16, z + 1 + dz * 16);
              const int sx = clamp && dx ? (dx < 0 ? 0 : 15) : x;
              const int sz = clamp && dz ? (dz < 0 ? 0 : 15) : z;
              const int si = SubChunk::index(sx, y, sz);
              BlockId b;
              if (under || !s) b = under ? pin->belowWorld : kAir;
              else if (w64) b = s->full[size_t(si)];
              else b = s->lut[s->data.blocks.rawIndex(size_t(si))];
              out.id[i] = b;
              out.st[i] = s ? s->data.state[size_t(si)] : 0;  // M9: apron state from the snapshots too
              out.mat[i] = s ? s->data.materialFlat(size_t(si)) : 0;
            }
      }
}

static bool verifyForcedOff() { static const bool v = [] { const char* e = std::getenv("LL_VERIFY_OFF"); return e && e[0] == '1'; }(); return v; }  // verify-off switch: LL_VERIFY_OFF=1 / --no-verify disables DebugOpt always-on verifies (benchmarks)
static bool cowVerifyOn() {
#if defined(LL_DEBUGOPT_BUILD) || !defined(NDEBUG)
  return !verifyForcedOff();
#else
  static const bool v = [] { const char* e = std::getenv("LL_COW_VERIFY"); return e && e[0] == '1'; }();
  return v;
#endif
}

bool sameMeshInput(const MeshInput& a, const MeshInput& b) {
  if (std::memcmp(a.id, b.id, sizeof a.id) || std::memcmp(a.st, b.st, sizeof a.st) || std::memcmp(a.mat, b.mat, sizeof a.mat))
    return false;
  if (a.matPaletteSize != b.matPaletteSize || std::memcmp(a.matPalette, b.matPalette, sizeof(uint16_t) * std::min<size_t>(a.matPaletteSize, 256)))
    return false;
  return a.text == b.text;
}

void pinForMeshingInto(const ChunkMap& w, const cBlockReg& reg, int cx, int sy, int cz, uint32_t epoch, uint64_t seq,
                       BlockId belowWorld, MeshInput& out, MeshEdge edge, uint64_t republishOwnMask) {
  out.cx = cx; out.sy = sy; out.cz = cz; out.epoch = epoch; out.seq = seq;
  if (!cowColumnsEnabled()) {
    out.pin.reset();
    snapshotForMeshingInto(w, reg, cx, sy, cz, epoch, seq, belowWorld, out, edge);
    return;
  }
  ColumnTable& t = w.cowTable();
  if (republishOwnMask) t.publish(w, reg, cx, cz, republishOwnMask);
  out.pin = t.pin(w, reg, cx, sy, cz, belowWorld, edge);
  if (cowVerifyOn()) {  // owner thread: gathered (pinned) == legacy live snapshot
    auto g = std::make_unique<MeshInput>();
    auto l = std::make_unique<MeshInput>();
    g->pin = out.pin;
    gatherPinned(*g);
    snapshotForMeshingInto(w, reg, cx, sy, cz, epoch, seq, belowWorld, *l, edge);
    const bool same = sameMeshInput(*g, *l);
    noteCowVerify(same);
    static std::atomic<int> logged{0};
    if (!same && logged.fetch_add(1) < 12) {
      int nId = 0, nSt = 0, nMat = 0, own = 0, first = -1;
      for (int i = 0; i < kMeshPadVolume; ++i) {
        const bool d = g->id[i] != l->id[i] || g->st[i] != l->st[i] || g->mat[i] != l->mat[i];
        nId += g->id[i] != l->id[i]; nSt += g->st[i] != l->st[i]; nMat += g->mat[i] != l->mat[i];
        if (d) { if (first < 0) first = i; const int x = i % kMeshPad, z = (i / kMeshPad) % kMeshPad, y = i / (kMeshPad * kMeshPad); if (x && z && y && x < 17 && y < 17 && z < 17) ++own; }
      }
      std::fprintf(stderr, "cow-verify MISMATCH (%d,%d,%d): id %d st %d mat %d own-cells %d first pad %d text %d pal %d\n", cx, sy, cz, nId, nSt, nMat, own, first,
                   int(g->text != l->text), int(g->matPaletteSize != l->matPaletteSize));
    }
  }
}

}  // namespace litholattice
