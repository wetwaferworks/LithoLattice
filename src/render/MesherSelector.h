// IUnifiedMesher refactor step 5 (UML_UNIFIED_MESHER_2026-10-30.md): the single place the
// mesher's LL_* environment switches are read. The env vars stay the inputs, with the same
// parse; each caller keeps its own caching and timing (static-once, atomic-once with a CLI
// setter, or uncached for LL_VERIFY_OFF), so behaviour is unchanged. config() snapshots the
// live switches through the public accessors. There is no IUnifiedMesher yet, so the
// UML create() factory waits for that interface.
#pragma once

namespace litholattice {

class cMesherSelector {
 public:
  cMesherSelector() = delete;

  /// The mesher switches as currently resolved (env defaults plus CLI setters).
  struct Config {
    bool unifiedMesher = false;
    bool meshVerify = false;
    bool leavesFancy = true;
    int foliageDensity = 0;
  };
  static Config config();

  static int envP2PerfOff();                    ///< LL_P2_PERF_OFF (strtol, base 0; unset -> 0)
  static int envGreedyOverride();               ///< LL_MESH_GREEDY: -1 unset/empty, 0 if '0', else 1
  static int envLeavesFancy();                  ///< LL_LEAVES: 0 if "fast", else 1
  static int envFoliageDensity();               ///< LL_FOLIAGE_DENSITY: 1 if "1", else 0
  static bool envVerifyOff();                   ///< LL_VERIFY_OFF starts with '1' (read live)
  static int envUnifiedMesher();                ///< LL_UNIFIED_MESHER starts with '1' -> 1, else 0
  static bool envCanonVerify();                 ///< LL_CANON_VERIFY starts with '1'
};

}  // namespace litholattice
