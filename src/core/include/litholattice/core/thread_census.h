// Thread Handshake P1: boot thread census and thread records (read-only).
//
// cThreadCensus reads std::thread::hardware_concurrency() ONCE (first use) and computes
// the thread budget for the two classes in Archie's design:
//   Class A (dedicated): main thread, Timing Handshake sim clock + shard workers,
//                        software-renderer bands. Stamped spawnTick=0, returned at
//                        power-down with reason "engine power-down".
//   Class B (pooled):    the background pool (lend / reclaim via an atomic ticket,
//                        POOLED -> CLAIMED -> RECLAIMED).
// On 4 or fewer cores the dedicated threads claim their cores first and the pool shrinks.
//
// P1 is RECORD-ONLY: nothing in the engine takes its counts from here yet (that is P2/P3),
// so no thread count and no hash changes. The budget mirrors today's own sizing rules
// (Timing Handshake min(4, hw-1); renderer bands clamp(hw-1, 2, 16)) so P2 can switch the
// owners over without changing behaviour.
//
// ThreadRecord keeps HASHED fields (ticks and enums only) apart from ThreadTelemetry
// (wall clock, OS ids), which is never hashed.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace litholattice {

enum class ThreadClass : uint8_t { Dedicated = 0 /*A*/, Pooled = 1 /*B*/ };

enum class ThreadRole : uint8_t {
  Main = 0, SimClock, SimShard, RenderBand, PoolWorker,
  Audio, Input, Storage, Mcp, Web, Other,  // "rogue" threads: declared, class TBD (Archie Q3)
};

/// Lifecycle. Dedicated: DEDICATED -> RETURNED. Pooled: POOLED -> CLAIMED -> RECLAIMED -> POOLED ...
enum class ThreadState : uint8_t { Dedicated = 0, Pooled, Claimed, Reclaimed, Returned };

enum class ThreadReturnReason : uint8_t { None = 0, EnginePowerDown, Reclaimed };

/// Pure budget for a given logical core count. computeThreadBudget is the single rule.
struct ThreadBudget {
  unsigned hwRaw = 0;          ///< hardware_concurrency() as reported (0 = unknown)
  unsigned hw = 1;             ///< max(1, hwRaw)
  bool smallMachine = false;   ///< hw <= 4: dedicated threads claim first, the pool shrinks
  // Class A
  unsigned mainThreads = 1;    ///< the caller / window thread (not spawned)
  unsigned simClock = 1;       ///< Timing Handshake clock thread
  unsigned simShards = 1;      ///< Timing Handshake shard workers: min(4, hw-1), floor 1
  unsigned renderBands = 2;    ///< software renderer bands incl. caller: clamp(hw-1, 2, 16)
  unsigned renderSpawned = 1;  ///< bands spawned as threads (caller runs one band)
  // Class B
  unsigned pool = 1;           ///< pooled background workers
  unsigned activeCap = 1;      ///< adaptive cap seed: max pooled workers active at once
  /// Cores the always-running dedicated threads hold (main + sim clock).
  unsigned dedicatedCores() const { return mainThreads + simClock; }
  /// Every OS thread the engine would own at boot (main excluded: it already exists).
  unsigned spawnedTotal() const { return simClock + simShards + renderSpawned + pool; }
};

ThreadBudget computeThreadBudget(unsigned hwRaw);

/// Hashed part of a thread record: tick stamps and enums only (no wall clock, no OS ids).
struct ThreadRecord {
  uint32_t id = 0;              ///< census slot, assigned in declaration order
  ThreadClass cls = ThreadClass::Dedicated;
  ThreadRole role = ThreadRole::Other;
  ThreadState state = ThreadState::Dedicated;
  ThreadReturnReason reason = ThreadReturnReason::None;
  uint16_t roleIndex = 0;       ///< e.g. shard 0..3, band 0..15
  uint64_t spawnTick = 0;       ///< Timing Handshake tick (Class A: always 0)
  uint64_t returnTick = 0;      ///< tick of the last return / reclaim
  uint32_t ticket = 0;          ///< lend count (Class B)
};

/// Never hashed: wall clock and OS identity, for logs and profiling only.
struct ThreadTelemetry {
  uint64_t osThreadId = 0;
  int64_t wallSpawnNs = 0;
  int64_t wallReturnNs = 0;
  std::string name;
};

/// FNV-1a over the hashed fields of one record (fixed field order, endian-independent).
uint64_t threadRecordHash(const ThreadRecord& r);

/// Process-wide census. budget() is fixed after the first call. Records are append-only.
class cThreadCensus {
 public:
  static cThreadCensus& instance();

  /// Census for an explicit core count (tests). instance() uses hardware_concurrency().
  explicit cThreadCensus(unsigned hwRaw);

  const ThreadBudget& budget() const { return budget_; }

  /// Declare a thread. Dedicated records start DEDICATED at spawnTick 0; pooled records
  /// start POOLED. Returns the record id.
  uint32_t declare(ThreadClass cls, ThreadRole role, uint16_t roleIndex, const char* name);

  /// Pooled lend / reclaim through the record's atomic ticket. tryClaim succeeds only
  /// from POOLED or RECLAIMED; reclaim only from CLAIMED. Returns false otherwise.
  bool tryClaim(uint32_t id, uint64_t tick);
  bool reclaim(uint32_t id, uint64_t tick);

  /// Power-down: every record becomes RETURNED with reason EnginePowerDown at tick.
  void returnAll(uint64_t tick);

  size_t size() const;
  ThreadRecord record(uint32_t id) const;
  ThreadTelemetry telemetry(uint32_t id) const;
  void setTelemetry(uint32_t id, const ThreadTelemetry& t);

  /// Hash of all records in id order (telemetry excluded).
  uint64_t hash() const;
  /// Count of records in a state.
  size_t count(ThreadState s) const;

  /// How many times hardware_concurrency() was read by instance() (expected: 1).
  static unsigned hwReads();

 private:
  struct Slot {
    ThreadRecord rec;
    ThreadTelemetry tel;
    std::atomic<uint64_t> word{0};  ///< [ticket:32 | state:8], the lend/reclaim ticket
  };
  ThreadBudget budget_;
  mutable std::mutex m_;  ///< guards slots_ growth and telemetry; hot path is the atomic word
  std::vector<std::unique_ptr<Slot>> slots_;
};

}  // namespace litholattice
