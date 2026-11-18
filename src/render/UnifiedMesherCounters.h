// Refactor step 4: the unified-mesher process counters (were g_um* globals in mesher.cpp).
// Same atomics, same zero start, same relaxed use; read by unifiedMesherStats().
#pragma once
#include <atomic>
#include <cstdint>

namespace litholattice {

class cUnifiedMesherCounters {
 public:
  cUnifiedMesherCounters() = delete;
  static inline std::atomic<uint64_t> meshes{0}, checked{0}, mismatch{0}, unpublished{0}, quads{0}, bytes{0};
  static inline std::atomic<uint64_t> fbVerts{0}, fbChecked{0}, fbMismatch{0};
  static inline std::atomic<uint64_t> splits{0};
};

}  // namespace litholattice
