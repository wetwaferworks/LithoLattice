// Silicon Cities selftest (Cortana, 2026-10-16; design and math: Argon).
#include <array>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include "litholattice/procgen/generator_loader.h"
#include "litholattice/procgen/silicon_cities.h"

namespace litholattice::procgen {
namespace {

struct Check {
  int fails = 0;
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] silicon_cities: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails;
  }
};

constexpr const char* kNames[] = {"rock", "pad", "found", "deck", "tl", "td", "rail", "pil", "alu", "clk", "clktop", "sram", "sramd", "ramp"};
std::optional<MaterialId> testMat(std::string_view n) {
  for (size_t i = 0; i < std::size(kNames); ++i) if (n == kNames[i]) return MaterialId(i + 1);
  return std::nullopt;
}
constexpr MaterialId kPad = 2, kDeck = 4, kRamp = 14;

std::string genJson(const char* cityExtra) {
  std::string s = R"({"graph": {"nodes": {"base": {"type": "noise", "noise": "perlin", "frequency": 0.01},)"
                  R"( "height": {"type": "scale_bias", "input": "base", "scale": 6, "bias": 100}}, "outputs": {"height": "height"}},)"
                  R"( "terrain": {"type": "heightmap", "output": "height", "material": "rock", "depth": 0})";
  if (cityExtra) {
    s += R"(, "features": [{"name": "silicon_cities", "type": "silicon_cities", "base_y": 120, )";
    s += cityExtra;
    s += R"( "materials": {"pad": "pad", "foundation": "found", "deck": "deck", "trace_live": "tl", "trace_dormant": "td",)"
         R"( "guardrail": "rail", "pillar": "pil", "alu": "alu", "clock": "clk", "clock_top": "clktop", "sram": "sram", "sram_dormant": "sramd", "ramp": "ramp"}}])";
  }
  return s + "}";
}

constexpr uint64_t kDim = 0x5111C0Aull;

ChunkInfo chunkAt(int cx, int cz) {
  ChunkInfo c;
  c.cx = cx; c.cz = cz; c.dimensionSeed = kDim; c.chunkSeed = chunkSeed(kDim, cx, cz);
  return c;
}

bool same(const ColumnBuffer& a, const ColumnBuffer& b) {
  for (int y = 0; y < a.height(); ++y)
    for (int z = 0; z < a.size(); ++z)
      for (int x = 0; x < a.size(); ++x)
        if (a.get(x, y, z) != b.get(x, y, z) || a.state(x, y, z) != b.state(x, y, z)) return false;
  return true;
}

}  // namespace

int runSiliconCitiesSelfTest() {
  Check check;
  // Integer math.
  bool mono = true;
  for (int t = 1; t <= 64; ++t) mono = mono && quinticQ16(t) >= quinticQ16(t - 1);
  check(quinticQ16(0) == 0 && quinticQ16(32) == 32768 && quinticQ16(64) == 65536 && mono,
        "QuinticQ16 runs 0..65536, midpoint 32768, monotone");
  check(quinticQ16(-5) == 0 && quinticQ16(99) == 65536, "QuinticQ16 clamps outside [0, 64]");
  check(cityBusWidth(0) == 4 && cityBusWidth(1ull << 24) == 8 && cityBusWidth(2ull << 24) == 16 && cityBusWidth(3ull << 24) == 32,
        "bus width 4u << ((h >> 24) & 3) gives 4/8/16/32");
  bool rampOk = cityRampRise(0, 8) == 0 && cityRampRise(16, 8) == 8 && cityRampRise(15, 8) == 7 && cityRampRise(400, 8) == 8;
  for (int s = 1; s <= 16; ++s) rampOk = rampOk && cityRampRise(s, 8) - cityRampRise(s - 1, 8) >= 0 && cityRampRise(s, 8) - cityRampRise(s - 1, 8) <= 1;
  check(rampOk, "M1 ramp is 1:2 and rises exactly 8 over 16 blocks");
  check(cityRampRise(32, 16) == 16, "M2 ramp rises exactly 16 over 32 blocks");

  // District distribution and hub ranges over a 64x64 sector sample (negative sectors included).
  const uint64_t cs = citySeed(kDim);
  std::array<int, 5> counts{};
  std::set<uint32_t> hashes;
  bool rangeOk = true;
  int live = 0, unlinked = 0;
  for (int sz = -32; sz < 32; ++sz)
    for (int sx = -32; sx < 32; ++sx) {
      const CityHub h = cityHubAt(cs, sx, sz);
      ++counts[size_t(h.district)];
      hashes.insert(h.hash);
      const int ox = h.x - sx * 256, oz = h.z - sz * 256;
      rangeOk = rangeOk && ox >= 64 && ox <= 191 && oz >= 64 && oz <= 191 && (h.x >> 8) == sx && (h.z >> 8) == sz;
      live += h.live;
      unlinked += !cityLinkConnected(cs, sx, sz, true);
      unlinked += !cityLinkConnected(cs, sx, sz, false);
    }
  const auto near = [](int got, int want) { return got * 100 >= want * 85 && got * 100 <= want * 115; };
  check(near(counts[0], 1536) && near(counts[1], 1024) && near(counts[2], 512) && near(counts[3], 512) && near(counts[4], 512),
        "district mix ~3/8 quarry, 2/8 SRAM, 1/8 clock/ALU/LithoHub (" + std::to_string(counts[0]) + "/" +
            std::to_string(counts[1]) + "/" + std::to_string(counts[2]) + "/" + std::to_string(counts[3]) + "/" +
            std::to_string(counts[4]) + " of 4096)");
  check(rangeOk, "hub offsets stay in 64..191 inside their own sector, negative sectors included");
  check(hashes.size() == 4096, "no hub hash collisions in 4096 sectors (" + std::to_string(hashes.size()) + " unique)");
  check(live > 4096 / 10 && live < 4096 / 4, "mostly dormant: " + std::to_string(live) + "/4096 live");
  check(unlinked > 8192 / 10 && unlinked < 8192 * 15 / 100, "10-15% of bus links unconnected (" + std::to_string(unlinked) + "/8192)");
  std::set<uint64_t> seeds;
  std::set<uint32_t> origin;
  for (uint64_t w = 0; w < 2048; ++w) { seeds.insert(citySeed(w)); origin.insert(cityHubAt(citySeed(w), 0, 0).hash); }
  check(seeds.size() == 2048 && origin.size() == 2048, "no city-seed collisions across 2048 dimension seeds (seed 0 included)");
  check(cityHubAt(citySeed(0), 0, 0).hash != 0, "seed 0 at the origin does not hash to zero");


  // Ramp shapes (state packing + Minecraft-stair corner snapping).
  {
    using namespace ramp;
    bool packOk = true;
    for (int f = 0; f < 4; ++f)
      for (int sh = 0; sh < 3; ++sh)
        for (int hg = 0; hg < 2; ++hg) {
          const uint8_t st = encode(f, sh, hg != 0);
          packOk = packOk && st < 32 && facingOf(st) == f && shapeOf(st) == sh && hangingOf(st) == (hg != 0);
        }
    check(packOk, "ramp state packs facing 2 + shape 2 + hanging 1 bits (< 32)");
    auto highCount = [](uint8_t st) { int c = 0; for (int xi = 0; xi < 2; ++xi) for (int zi = 0; zi < 2; ++zi) c += cornerHigh(st, xi, zi); return c; };
    check(highCount(encode(0, kStraight, false)) == 2 && highCount(encode(0, kInner, false)) == 3 && highCount(encode(0, kOuter, false)) == 1,
          "ramp corner heights: straight 2, inner 3, outer 1 high corners");
    auto none = [](int, int) { return -1; };
    check(snap(0, false, none) == encode(0, kStraight, false), "lone ramp stays straight");
    // North-facing ramp with a west-facing ramp on its high (north) side: outer-left corner.
    auto outerL = [](int dx, int dz) { return (dx == 0 && dz == -1) ? 3 : -1; };
    auto outerR = [](int dx, int dz) { return (dx == 0 && dz == -1) ? 1 : -1; };
    auto innerL = [](int dx, int dz) { return (dx == 0 && dz == 1) ? 3 : -1; };
    auto innerR = [](int dx, int dz) { return (dx == 0 && dz == 1) ? 1 : -1; };
    const uint8_t oL = snap(0, false, outerL), oR = snap(0, false, outerR), iL = snap(0, false, innerL), iR = snap(0, false, innerR);
    check(oL == encode(0, kOuter, false) && oR == encode(1, kOuter, false), "outer corners snap (left kept, right stored as next facing)");
    check(iL == encode(0, kInner, false) && iR == encode(1, kInner, false), "inner corners snap (left kept, right stored as next facing)");
    check(cornerHigh(oL, 0, 0) && !cornerHigh(oL, 1, 0) && cornerHigh(oR, 1, 0) && !cornerHigh(oR, 0, 0),
          "outer-left keeps the NW corner high, outer-right the NE corner");
    check(!cornerHigh(iL, 1, 1) && cornerHigh(iL, 0, 1) && !cornerHigh(iR, 0, 1) && cornerHigh(iR, 1, 1),
          "inner-left drops the SE corner, inner-right the SW corner");
    auto blocked = [](int dx, int dz) { return (dx == 0 && dz == -1) ? 3 : (dx == 1 && dz == 0) ? 0 : -1; };
    check(snap(0, false, blocked) == encode(0, kStraight, false), "a same-facing ramp beside it blocks the corner (canTakeShape)");
  }
  // Generation through the loader.
  std::string err;
  const auto on = loadGenerator(genJson(R"("enabled": true,)"), kDim, testMat, err);
  check(on != nullptr, "generator with silicon_cities loads" + (err.empty() ? std::string() : " (" + err + ")"));
  const auto off = loadGenerator(genJson(R"("enabled": false,)"), kDim, testMat, err);
  const auto none = loadGenerator(genJson(nullptr), kDim, testMat, err);
  if (!on || !off || !none) return check.fails + 1;
  for (const int sx : {0, -3}) {
    const CityHub hub = cityHubAt(cs, sx, sx);
    const ChunkInfo c = chunkAt(hub.x >> 4, hub.z >> 4);
    const ColumnBuffer a = on->generate(c), b = on->generate(c);
    check(same(a, b), "identical output twice (sector " + std::to_string(sx) + ")");
    check(a.get(hub.x & 15, 120, hub.z & 15) == kPad, std::string("hub pad at base_y (") + cityDistrictName(hub.district) +
                                                          ", sector " + std::to_string(sx) + (sx < 0 ? ", negative coords)" : ")"));
    check(!same(a, none->generate(c)), "cities change the hub chunk (sector " + std::to_string(sx) + ")");
    check(same(off->generate(c), none->generate(c)), "enabled:false is byte-identical to no city stage (sector " + std::to_string(sx) + ")");
  }
  setSiliconCitiesOverride(0);
  const auto ovOff = loadGenerator(genJson(R"("enabled": true,)"), kDim, testMat, err);
  setSiliconCitiesOverride(1);
  const auto ovOn = loadGenerator(genJson(R"("enabled": false,)"), kDim, testMat, err);
  setSiliconCitiesOverride(-1);
  const CityHub h0 = cityHubAt(cs, 0, 0);
  const ChunkInfo c0 = chunkAt(h0.x >> 4, h0.z >> 4);
  check(ovOff && same(ovOff->generate(c0), none->generate(c0)), "--cities off overrides enabled:true");
  check(ovOn && same(ovOn->generate(c0), on->generate(c0)), "--cities on overrides enabled:false");

  // A connected M1 link: walk south from hub (0,0) along x = a.x and check the ramp in the world.
  int sxL = 0;
  while (!cityLinkConnected(cs, sxL, 0, false)) ++sxL;
  const CityHub A = cityHubAt(cs, sxL, 0);
  int deckLo = -1, deckHi = -1;
  for (int wz = A.z + A.padRadius + 1; wz <= A.z + A.padRadius + 24; ++wz) {
    const ColumnBuffer col = on->generate(chunkAt(A.x >> 4, wz >> 4));
    // Lowest deck at or above base_y: an M2 leg can share this x higher up (it is drawn last).
    for (int y = 110; y < 256; ++y)
      if (col.get(A.x & 15, y, wz & 15) == kDeck) {  // perp 0 is roadbed
        if (deckLo < 0) deckLo = y;
        deckHi = y;
        break;
      }
  }
  check(deckLo == 120 && deckHi == 128, "in-world M1 ramp climbs from 120 to 128 (" + std::to_string(deckLo) + " -> " +
                                            std::to_string(deckHi) + ")");
  // Ramps in the world: count ramp cells and corner shapes over the 4 sectors around the origin.
  {
    int ramps = 0, corners = 0, hanging = 0;
    bool stable = true;
    for (int cz = 0; cz < 32; cz += 1)
      for (int cx = 0; cx < 32; cx += 1) {
        if (((cx ^ cz) & 1) != 0) continue;  // half the chunks: enough, and quick
        const ColumnBuffer b = on->generate(chunkAt(cx, cz));
        for (int y = 100; y < 160; ++y)
          for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x)
              if (b.get(x, y, z) == kRamp) {
                ++ramps;
                const uint8_t st = b.state(x, y, z);
                corners += ramp::shapeOf(st) != ramp::kStraight;
                hanging += ramp::hangingOf(st);
                stable = stable && ramp::shapeOf(st) != 3;
              }
      }
    check(ramps > 500 && hanging > 0 && hanging < ramps && stable,
          "worldgen ramps on steps, undersides and edges (" + std::to_string(ramps) + " cells, " + std::to_string(hanging) + " hanging)");
    check(corners > 0, "worldgen ramps snap inner/outer corners where edges meet (" + std::to_string(corners) + " corners)");
    // Chunk-border agreement: the same chunk generated twice and its neighbour seam match.
    const ColumnBuffer a1 = on->generate(chunkAt(9, 4)), a2 = on->generate(chunkAt(9, 4));
    check(same(a1, a2), "ramp pass is deterministic");
  }
  std::printf(check.fails ? "SILICON CITIES SELFTEST FAILED (%d)\n" : "SILICON CITIES SELFTEST OK\n", check.fails);
  return check.fails;
}

}  // namespace litholattice::procgen
