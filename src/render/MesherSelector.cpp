#include "MesherSelector.h"

#include <cstdlib>
#include <string_view>

#include "litholattice/render/mesher.h"

namespace litholattice {

cMesherSelector::Config cMesherSelector::config() {
  Config c;
  c.unifiedMesher = unifiedMesherEnabled();
  c.meshVerify = meshVerifyEnabled();
  c.leavesFancy = leavesFancy();
  c.foliageDensity = foliageDensity();
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

bool cMesherSelector::envCanonVerify() {
  const char* e = std::getenv("LL_CANON_VERIFY");
  return e && e[0] == '1';
}

}  // namespace litholattice
