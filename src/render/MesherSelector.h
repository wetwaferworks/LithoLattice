// IUnifiedMesher refactor step 5 (UML_UNIFIED_MESHER_2026-10-30.md): the single place the
// mesher's LL_* environment switches are read. The env vars stay the inputs, with the same
// parse; each caller keeps its own caching and timing (static-once, atomic-once with a CLI
// setter, or uncached for LL_VERIFY_OFF), so behaviour is unchanged. config() snapshots the
// live switches through the public accessors. There is no IUnifiedMesher yet, so the
// UML create() factory waits for that interface.
#pragma once
#include <utility>

namespace litholattice {

class cMesherSelector {
 public:
  cMesherSelector() = delete;

  /// The mesher switches as currently resolved (env defaults plus CLI setters).
  struct Config {
    bool unifiedMesher = false;
    bool meshVerify = false;
    bool canonLods = true;
    bool leavesFancy = true;
    int foliageDensity = 0;
    int lodRing1 = 12, lodRing2 = 24;
  };
  static Config config();

  static int envP2PerfOff();                    ///< LL_P2_PERF_OFF (strtol, base 0; unset -> 0)
  static int envGreedyOverride();               ///< LL_MESH_GREEDY: -1 unset/empty, 0 if '0', else 1
  static int envLeavesFancy();                  ///< LL_LEAVES: 0 if "fast", else 1
  static int envFoliageDensity();               ///< LL_FOLIAGE_DENSITY: 1 if "1", else 0
  static bool envVerifyOff();                   ///< LL_VERIFY_OFF starts with '1' (read live)
  static int envUnifiedMesher();                ///< LL_UNIFIED_MESHER starts with '1' -> 1, else 0
  static std::pair<int, int> envCanonLodRings();///< LL_CANON_LOD_RINGS "a,b" (0 < a < b), else {12, 24}
  static bool envCanonLods();                   ///< LL_CANON_LODS: false only if it starts with '0'
  static bool envCanonVerify();                 ///< LL_CANON_VERIFY starts with '1'
};

}  // namespace litholattice
