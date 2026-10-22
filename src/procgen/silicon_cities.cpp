// Silicon Cities stage (design and math: Argon; implementation: Cortana, 2026-10-16).
// Integer math only: no floats, no <cmath>. See silicon_cities.h for the rules.
#include "litholattice/procgen/silicon_cities.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <vector>
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

std::atomic<int> gOverride{-1};

constexpr int iabs(int v) { return v < 0 ? -v : v; }

struct RampOp {
  int y;
  uint8_t facing;
  bool hanging;
  int8_t half = -1;  ///< Highway Ramp half (0 low, 1 high); -1 = Aluminum Ramp (JD 2026-10-16)
};

struct Link {
  CityHub a, b;
  bool eastWest = true;  ///< true: M2 (+16) EW link; false: M1 (+8) NS link
  bool live = false;
};

/// Writes column (lx, lz) of the buffer, tracking the highest solid y.
struct ColumnWriter {
  ColumnBuffer& b;
  int lx, lz, top;
  void set(int y, MaterialId m) {
    if (y < 0 || y >= b.height()) return;
    b.set(lx, y, lz, m);
    if (m != kAirMaterial) top = std::max(top, y);
  }
  void fill(int y0, int y1, MaterialId m) { for (int y = y0; y <= y1; ++y) set(y, m); }
  void clear(int y0, int y1) {
    for (int y = std::max(0, y0); y <= y1 && y < b.height(); ++y) b.set(lx, y, lz, kAirMaterial);
  }
};

/// Deck height of one leg at coordinate `along` (no perpendicular test), or -1 off the leg or
/// inside a hub pad. `from..to` is the leg span on its axis, `s0` the route distance from hub A
/// where the leg starts.
int legDeckY(const SiliconCitySettings& s, const Link& L, int along, int from, int to, int s0, int routeLen) {
  const int lo = std::min(from, to) - 8, hi = std::max(from, to) + 8;
  if (along < lo || along > hi) return -1;
  const int sA = s0 + std::clamp(iabs(along - from), 0, iabs(to - from));
  const int sB = routeLen - sA;
  // Pads own their footprint: the deck starts at the pad edge.
  if (sA <= L.a.padRadius || sB <= L.b.padRadius) return -1;
  const int rise = L.eastWest ? 16 : 8;
  return s.baseY + std::min(cityRampRise(sA - L.a.padRadius, rise), cityRampRise(sB - L.b.padRadius, rise));
}

/// Deck for one leg of a link at this column, if the column lies on it.
void deckLeg(const SiliconCitySettings& s, const Link& L, ColumnWriter& w, int h, int along, int perp,
             int from, int to, int s0, int routeLen) {
  if (iabs(perp) > 8) return;
  const int y = legDeckY(s, L, along, from, to, s0, routeLen);
  if (y < 0) return;
  const int d = iabs(perp);
  if (d < 8) w.clear(y + 1, y + 4);
  const bool channel = d == 2 || d == 4 || d == 6;
  w.set(y, channel ? (L.live ? s.traceLive : s.traceDormant) : s.deck);
  if (d == 8) w.set(y + 1, s.guardrail);
  // Pillars every 16 blocks along the leg, at the deck edges.
  if (d == 7 && (along & 15) == 0 && y - 1 > h) w.fill(h + 1, y - 1, s.pillar);
}

/// Ramp blocks for one leg at this column (terrain-independent, so neighbours can be evaluated
/// anywhere for corner snapping). `plus` / `minus`: facings of +along / -along; `perpPlus`:
/// facing of +perp.
void rampLeg(const SiliconCitySettings& s, const Link& L, int along, int perp, int from, int to, int s0, int routeLen,
             int plus, int perpPlus, std::vector<RampOp>& out) {
  const int d = iabs(perp);
  if (d > 8) return;
  const int y = legDeckY(s, L, along, from, to, s0, routeLen);
  if (y < 0) return;
  const int minus = (plus + 2) & 3;
  if (d == 8) {  // under-edge bracket, full side toward the road centre
    out.push_back({y - 1, uint8_t(perp > 0 ? (perpPlus + 2) & 3 : perpPlus), true});
    return;
  }
  const int yp = legDeckY(s, L, along + 1, from, to, s0, routeLen), ym = legDeckY(s, L, along - 1, from, to, s0, routeLen);
  if (s.ramp2) {  // Highway Ramp (Proposed by JD, Built by Cortana): one 2-cell pair per 1:2 step
    const int yp2 = legDeckY(s, L, along + 2, from, to, s0, routeLen), ym2 = legDeckY(s, L, along - 2, from, to, s0, routeLen);
    if (yp == y + 1 && ym == y) out.push_back({y + 1, uint8_t(plus), false, 1});
    else if (yp == y && yp2 == y + 1) out.push_back({y + 1, uint8_t(plus), false, 0});
    else if (ym == y + 1 && yp == y) out.push_back({y + 1, uint8_t(minus), false, 1});
    else if (ym == y && ym2 == y + 1) out.push_back({y + 1, uint8_t(minus), false, 0});
    else if (yp == y + 1) out.push_back({y + 1, uint8_t(plus), false});  // unpaired step: 1-block ramp
    else if (ym == y + 1) out.push_back({y + 1, uint8_t(minus), false});
  } else if (yp == y + 1) out.push_back({y + 1, uint8_t(plus), false});        // top of a step
  else if (ym == y + 1) out.push_back({y + 1, uint8_t(minus), false});
  if (ym == y - 1) out.push_back({y - 1, uint8_t(minus), true});        // underside of a step
  else if (yp == y - 1) out.push_back({y - 1, uint8_t(plus), true});
}

void deckLink(const SiliconCitySettings& s, const Link& L, ColumnWriter& w, int h, int wx, int wz) {
  const int lenX = iabs(L.b.x - L.a.x), lenZ = iabs(L.b.z - L.a.z), len = lenX + lenZ;
  if (L.eastWest) {  // x first at z = a.z, then z at x = b.x
    deckLeg(s, L, w, h, wx, wz - L.a.z, L.a.x, L.b.x, 0, len);
    deckLeg(s, L, w, h, wz, wx - L.b.x, L.a.z, L.b.z, lenX, len);
  } else {           // z first at x = a.x, then x at z = b.z
    deckLeg(s, L, w, h, wz, wx - L.a.x, L.a.z, L.b.z, 0, len);
    deckLeg(s, L, w, h, wx, wz - L.b.z, L.a.x, L.b.x, lenZ, len);
  }
}

void rampLink(const SiliconCitySettings& s, const Link& L, int wx, int wz, std::vector<RampOp>& out) {
  const int lenX = iabs(L.b.x - L.a.x), lenZ = iabs(L.b.z - L.a.z), len = lenX + lenZ;
  // Facings: N 0 (-Z), E 1 (+X), S 2 (+Z), W 3 (-X).
  if (L.eastWest) {
    rampLeg(s, L, wx, wz - L.a.z, L.a.x, L.b.x, 0, len, 1, 2, out);
    rampLeg(s, L, wz, wx - L.b.x, L.a.z, L.b.z, lenX, len, 2, 1, out);
  } else {
    rampLeg(s, L, wz, wx - L.a.x, L.a.z, L.b.z, 0, len, 2, 1, out);
    rampLeg(s, L, wx, wz - L.b.z, L.a.x, L.b.x, lenZ, len, 1, 2, out);
  }
}

void hubColumn(const SiliconCitySettings& s, const CityHub& hub, ColumnWriter& w, int h, int wx, int wz) {
  const int dx = wx - hub.x, dz = wz - hub.z, c = std::max(iabs(dx), iabs(dz));
  const int R = hub.padRadius, outer = R + s.skirt;
  if (c > outer) return;
  int top = s.baseY;
  if (c > R) {  // terraced skirt: QuinticQ16 blend from the pad down/up to the terrain
    const int t = ((outer - c) * 64) / s.skirt;
    top = h + int((int64_t(s.baseY - h) * quinticQ16(t)) >> 16);
    top &= ~3;
  }
  if (top > h) w.fill(h + 1, top - 1, s.foundation);
  w.set(top, c > R ? s.foundation : s.pad);
  w.top = top;  // a lowered pad lowers the surface
  w.clear(top + 1, std::max(h, top) + (c > R ? 4 : s.clearAbove));
  if (c > R) return;
  const int y0 = s.baseY + 1;
  switch (hub.district) {
    case CityDistrict::Alu:  // step pyramid: 2-wide, 2-tall tiers
      if (c <= 10) w.fill(y0, y0 + ((10 - c) / 2 + 1) * 2 - 1, s.alu);
      break;
    case CityDistrict::Clock:  // spire
      if (c <= 1) w.fill(y0, y0 + 31, s.clock);
      if (c == 0) { w.fill(y0 + 32, y0 + 39, s.clock); w.set(y0 + 40, s.clockTop); }
      break;
    case CityDistrict::Sram:  // 3x3 cell blocks on a 6-block grid
      if (c <= R - 2 && ((dx + 66) % 6) < 3 && ((dz + 66) % 6) < 3)
        w.fill(y0, y0 + 3, hub.live ? s.sram : s.sramDormant);
      break;
    case CityDistrict::LithoHub:  // beacon post
      if (c == 0) { w.fill(y0, y0 + 5, s.pillar); w.set(y0 + 6, s.clockTop); }
      break;
    case CityDistrict::Quarry:  // foundation pad only (placeholder)
      break;
  }
}

}  // namespace

const char* cityDistrictName(CityDistrict d) {
  switch (d) {
    case CityDistrict::Quarry: return "quarry";
    case CityDistrict::Sram: return "sram";
    case CityDistrict::Clock: return "clock";
    case CityDistrict::Alu: return "alu";
    case CityDistrict::LithoHub: return "lithohub";
  }
  return "?";
}

uint64_t citySeed(uint64_t dimensionSeed) { return deriveSeed(dimensionSeed, nameHash("city")); }

CityHub cityHubAt(uint64_t seed, int32_t sx, int32_t sz) {
  CityHub hub;
  hub.sx = sx;
  hub.sz = sz;
  const uint32_t h = latticeHash(seed, sx, sz);
  hub.hash = h;
  hub.x = sx * 256 + 64 + int32_t(h & 127u);          // 64..191
  hub.z = sz * 256 + 64 + int32_t((h >> 8) & 127u);   // 64..191
  hub.busWidth = cityBusWidth(h);                      // bits 24..25
  hub.district = kCityDistrictTable[(h >> 16) & 7u];   // bits 16..18
  hub.padRadius = 12 + int(hub.busWidth / 2u);         // 14, 16, 20, 28
  hub.live = ((h >> 28) & 15u) < 3u;                   // ~3/16 live
  return hub;
}

bool cityLinkConnected(uint64_t seed, int32_t sx, int32_t sz, bool eastWest) {
  const uint32_t h = latticeHash(seed ^ (eastWest ? 0x45ull : 0x4Eull), sx, sz, 1);
  return (h & 7u) >= 1u;  // 1 in 8 left unconnected
}

namespace {
/// The bus links that can touch sector (sx, sz), M1 before M2 (M2 drawn last wins crossings).
std::vector<Link> linksFor(uint64_t seed, int32_t sx, int32_t sz) {
  std::vector<Link> links;
  auto addLink = [&](int32_t ax, int32_t az, bool ew) {
    if (!cityLinkConnected(seed, ax, az, ew)) return;
    Link L;
    L.a = cityHubAt(seed, ax, az);
    L.b = cityHubAt(seed, ax + (ew ? 1 : 0), az + (ew ? 0 : 1));
    L.eastWest = ew;
    L.live = L.a.live || L.b.live;
    links.push_back(L);
  };
  addLink(sx, sz - 1, false);
  addLink(sx, sz, false);
  addLink(sx - 1, sz, true);
  addLink(sx, sz, true);
  return links;
}
}  // namespace

void SiliconCitiesFeature::place(StageContext& ctx, Rng& rng) const {
  (void)rng;  // pure function of (dimension seed, x, z)
  const int n = ctx.chunk.size;
  const uint64_t seed = citySeed(ctx.chunk.dimensionSeed);
  const int32_t sx = ctx.chunk.worldX(0) >> 8, sz = ctx.chunk.worldZ(0) >> 8;
  const CityHub hub = cityHubAt(seed, sx, sz);
  const std::vector<Link> links = linksFor(seed, sx, sz);
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const int wx = ctx.chunk.worldX(lx), wz = ctx.chunk.worldZ(lz);
      const size_t col = size_t(lx + lz * n);
      const int h = ctx.maps.height.empty() ? 0 : ctx.maps.height[col];
      ColumnWriter w{ctx.blocks, lx, lz, h};
      hubColumn(s_, hub, w, h, wx, wz);
      const int ground = w.top;
      for (const Link& L : links) deckLink(s_, L, w, ground, wx, wz);
      if (!ctx.maps.height.empty()) ctx.maps.height[col] = w.top;
    }
  if (!s_.ramp) return;
  // Ramp pass (after every deck write, so ramps win): smooth step tops, step undersides and deck
  // edges, with Minecraft-stair corner snapping. Neighbour ramps come from the same pure layout
  // function (neighbour sectors included), so shapes agree across chunk and sector borders.
  int32_t cacheSx[2] = {sx, INT32_MIN}, cacheSz[2] = {sz, INT32_MIN};
  std::vector<Link> cacheLinks[2] = {links, {}};
  auto linksAt = [&](int32_t qx, int32_t qz) -> const std::vector<Link>& {
    const int32_t ssx = qx >> 8, ssz = qz >> 8;
    for (int i = 0; i < 2; ++i) if (cacheSx[i] == ssx && cacheSz[i] == ssz) return cacheLinks[i];
    cacheSx[1] = ssx; cacheSz[1] = ssz; cacheLinks[1] = linksFor(seed, ssx, ssz);
    return cacheLinks[1];
  };
  auto rampsAt = [&](int32_t qx, int32_t qz, std::vector<RampOp>& out) {
    out.clear();
    for (const Link& L : linksAt(qx, qz)) rampLink(s_, L, qx, qz, out);
  };
  std::vector<RampOp> here, nb;
  for (int lz = 0; lz < n; ++lz)
    for (int lx = 0; lx < n; ++lx) {
      const int wx = ctx.chunk.worldX(lx), wz = ctx.chunk.worldZ(lz);
      rampsAt(wx, wz, here);
      if (here.empty()) continue;
      ColumnWriter w{ctx.blocks, lx, lz, ctx.maps.height.empty() ? 0 : ctx.maps.height[size_t(lx + lz * n)]};
      for (size_t i = 0; i < here.size(); ++i) {
        const RampOp op = here[i];
        bool later = false;  // a later op at the same y overwrites this one
        for (size_t k = i + 1; k < here.size(); ++k) later = later || here[k].y == op.y;
        if (later || op.y < 0 || op.y >= ctx.blocks.height()) continue;
        auto at = [&](int dx, int dz) {
          rampsAt(wx + dx, wz + dz, nb);
          int f = -1;
          for (const RampOp& q : nb) {
            if (q.y != op.y) continue;
            f = (q.half < 0 && q.hanging == op.hanging) ? int(q.facing) : -1;  // last write wins
          }
          return f;
        };
        if (op.half >= 0) {  // Highway Ramp: straight pair, never snapped
          w.set(op.y, s_.ramp2);
          ctx.blocks.setState(lx, op.y, lz, ramp::highway::encode(op.facing, op.half));
          continue;
        }
        const uint8_t st = ramp::snap(op.facing, op.half < 0 ? op.hanging : false, at);
        w.set(op.y, s_.ramp);
        ctx.blocks.setState(lx, op.y, lz, st);
      }
      if (!ctx.maps.height.empty()) ctx.maps.height[size_t(lx + lz * n)] = w.top;
    }
}

void setSiliconCitiesOverride(int v) { gOverride.store(v < 0 ? -1 : (v ? 1 : 0)); }
int siliconCitiesOverride() { return gOverride.load(); }

}  // namespace litholattice::procgen
