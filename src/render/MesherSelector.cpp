#include "MesherSelector.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "litholattice/render/mesher.h"

namespace litholattice {

cMesherSelector::Config cMesherSelector::config() {
  Config c;
  c.unifiedMesher = unifiedMesherEnabled();
  c.meshVerify = meshVerifyEnabled();
  c.canonLods = canonLodsEnabled();
  c.leavesFancy = leavesFancy();
  c.foliageDensity = foliageDensity();
  canonLodRings(c.lodRing1, c.lodRing2);
  return c;
}

int cMesherSelector::envP2PerfOff() {
  const char* e = std::getenv("LL_P2_PERF_OFF");
  return e ? int(std::strtol(e, nullptr, 0)) : 0;
}

int cMesherSelector::envGreedyOverride() {
  const char* e = std::getenv("LL_MESH_GREEDY");
  return e && *e ? (e[0] == '0' ? 0 : 1) : -1;
}

int cMesherSelector::envLeavesFancy() {
  const char* e = std::getenv("LL_LEAVES");
  return (e && std::string_view(e) == "fast") ? 0 : 1;
}

int cMesherSelector::envFoliageDensity() {
  const char* e = std::getenv("LL_FOLIAGE_DENSITY");
  return (e && std::string_view(e) == "1") ? 1 : 0;
}

bool cMesherSelector::envVerifyOff() {
  const char* e = std::getenv("LL_VERIFY_OFF");
  return e && e[0] == '1';
}

int cMesherSelector::envUnifiedMesher() {
  const char* e = std::getenv("LL_UNIFIED_MESHER");
  return (e && e[0] == '1') ? 1 : 0;
}

std::pair<int, int> cMesherSelector::envCanonLodRings() {
  int a = 12, b = 24;
  if (const char* e = std::getenv("LL_CANON_LOD_RINGS")) {
    int x = 0, y = 0;
    if (std::sscanf(e, "%d,%d", &x, &y) == 2 && x > 0 && y > x) a = x, b = y;
  }
  return std::pair<int, int>{a, b};
}

bool cMesherSelector::envCanonLods() {
  const char* e = std::getenv("LL_CANON_LODS");
  return !(e && e[0] == '0');
}

bool cMesherSelector::envCanonVerify() {
  const char* e = std::getenv("LL_CANON_VERIFY");
  return e && e[0] == '1';
}

}  // namespace litholattice
