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
#include <string>
#include <vector>
#include "litholattice/core/atomic_gate.h"

namespace litholattice {

enum class ThreadClass : uint8_t {
  Dedicated = 0 /*A*/, Pooled = 1 /*B*/,
  System = 2,  ///< P2 default (Archie Q3 open): background I/O roles, recorded but outside the budget
};

enum class ThreadRole : uint8_t {
  Main = 0, SimClock, SimShard, RenderBand, PoolWorker,
  Audio, Input, Storage, Mcp, Web, Other,  // background I/O: class System (P2 default, Archie Q3)
  RenderExecutor,  // detached-mode software renderer executor (P2)
};

/// Lifecycle. Dedicated: DEDICATED -> RETURNED. Pooled: POOLED -> CLAIMED -> RECLAIMED -> POOLED ...
enum class ThreadState : uint8_t { Dedicated = 0, Pooled, Claimed, Reclaimed, Returned };

enum class ThreadReturnReason : uint8_t { None = 0, EnginePowerDown, Reclaimed };

constexpr unsigned kMaxPoolWorkers = 64;
constexpr unsigned kMaxRenderBands = 64;  ///< explicit --render-threads ceiling (was kSwMaxRenderThreads)

/// Ruling 5 (t3340u): precedence CLI > LITHO_CORES env > hardware.
enum class CoresSource : uint8_t { Hardware = 0, Env, Cli };
struct CoresResolution { unsigned cores = 1; CoresSource source = CoresSource::Hardware; };
/// Pure: cli 0 = not given; env nullptr/empty/invalid/0 = not given; hw 0 = unknown (-> 1).
CoresResolution resolveCores(unsigned cli, const char* env, unsigned hw);

/// Ruling 5 + JD 2026-11-14: auto bands come from planRenderSide() so they share the
/// render-side budget with mesh/vertex/reserve workers. req 0 = auto. An explicit req
/// is clamped to the plan's raster slice (and kMaxRenderBands), never past the budget.
unsigned autoRenderBands(unsigned cores, bool wasm);
unsigned resolveRenderBands(unsigned req, unsigned cores, bool wasm);
#ifdef __wasm__
constexpr bool kCensusWasm = true;
#else
constexpr bool kCensusWasm = false;
#endif

/// JD 2026-11-14 / t46u 2026-11-16: after the permanent three (main, timing-handshake+control,
/// tick/sim), remaining = max(hw-3, 1). All render / vertex / mesh OS threads share exactly
/// that remainder, no +2 oversubscription any more (TheReefs 4 hw -> 1, so the software
/// renderer runs in 4 threads total; 12-hw workstation -> 9).
unsigned remainingAfterPermanentThree(unsigned hw);
unsigned renderSideBudget(unsigned hw);

/// One allocation used by the census, the shared/reserve pool, the dedicated mesh
/// pool, and the software raster bands (including a detached threeway executor).
struct RenderSidePlan {
  unsigned hw = 1;
  unsigned remaining = 1;          ///< max(hw-3, 1)
  unsigned budget = 1;             ///< == remaining (t46u: no oversubscription)
  unsigned meshDedicated = 0;      ///< dedicated mesh/vertex workers
  unsigned reservePool = 0;        ///< shared job-pool workers (render-side reserve)
  unsigned swBands = 1;            ///< software bands, caller included
  unsigned swSpawned = 0;          ///< extra band workers when the caller is main
  unsigned swSpawnedDetached = 0;  ///< executor + extra band workers (threeway software)
  unsigned usedSync() const { return meshDedicated + reservePool + swSpawned; }
  unsigned usedDetached() const { return meshDedicated + reservePool + swSpawnedDetached; }
};
/// LK_RENDER_THREADS=1: opt-in four-thread-total plan (1 software band, mesh
/// caller-runs, reserve 0). Default (unset) is the usual budget split.
RenderSidePlan planRenderSide(unsigned hw, bool wasm = kCensusWasm);

/// Pure budget for a given logical core count. computeThreadBudget is the single rule.
struct ThreadBudget {
  unsigned hwRaw = 0;          ///< hardware_concurrency() as reported (0 = unknown)
  unsigned hw = 1;             ///< max(1, hwRaw)
  bool smallMachine = false;   ///< hw <= 4: dedicated threads claim first, the pool shrinks
  // Class A
  unsigned mainThreads = 1;    ///< the caller / window thread (not spawned)
  unsigned simClock = 1;       ///< Timing Handshake clock thread (tick/sim)
  unsigned simShards = 1;      ///< Timing Handshake shard workers: min(4, hw-1), floor 1 (jobs, not OS threads)
  unsigned renderBands = 1;    ///< software renderer bands incl. caller (from planRenderSide)
  unsigned renderSpawned = 0;  ///< band OS threads counted against the render-side budget
  // Class B: reserve/shared pool, sized from planRenderSide (no longer clamp(cores-2)).
  unsigned pool = 1;           ///< pooled workers (single physical backing: cJobPool, P3)
  unsigned activeCap = 1;      ///< adaptive cap seed: max pooled workers active at once
  CoresSource source = CoresSource::Hardware;  ///< where hw came from (ruling 5)
  bool capped = false;           ///< render-side budget is always capped (JD 2026-11-14)
  unsigned dedicatedExtra = 0;   ///< dedicated threads beyond main + sim clock (control/event)
  bool layoutOriginal = false;
  unsigned remaining = 1;        ///< max(hw-3, 1) after the permanent three
  unsigned renderSideBudget = 1; ///< == remaining (t46u: no oversubscription)
  unsigned lfWorkers = 0;        ///< cLfJobQueue workers: whatever core slots are still free (t46u)
  unsigned meshDedicated = 0;    ///< dedicated mesh/vertex workers (0 if folded into pool)
  /// Cores the always-running dedicated threads hold (main + sim clock).
  unsigned dedicatedCores() const { return mainThreads + simClock; }
  /// Every OS thread the engine would own at boot (main excluded: it already exists).
  /// P3 (ruling 2): shards are pool jobs, not threads, so they no longer add to the spawn count.
  /// meshDedicated is spawned by the game when LL_DED_LAYOUT=6|8.
  unsigned spawnedTotal() const { return simClock + renderSpawned + pool + meshDedicated; }
  /// Render/vertex/mesh OS threads (must be <= renderSideBudget).
  unsigned renderSideUsed() const { return meshDedicated + pool + renderSpawned; }
  /// t46u: every compute thread the allocator hands a core slot to (permanent 3 included).
  unsigned slotsUsed() const { return 3u + renderSideUsed() + lfWorkers; }
};

/// t46u (JD 2026-11-16) thread allocator: the census is the one place that decides thread
/// counts. Callers ask it for the core count instead of std::thread::hardware_concurrency(),
/// so --cores / LITHO_CORES is honoured everywhere (software renderer, LF queue, pools, the
/// small-machine quality defaults). Boots the census on first use.
unsigned censusCores();
/// Human-readable slot plan: slot 0 main, 1 control/handshake, 2 tick/sim, then the
/// render-side roles in order. Logged once at census boot.
std::string slotPlan(const ThreadBudget& b);

ThreadBudget computeThreadBudget(unsigned hwRaw, bool wasm = kCensusWasm);

/// JD 2026-11-14: compute/render-side workers are mesh + reserve pool + spawned render
/// bands/executor. The permanent three (main, handshake+control, tick/sim) and System / I/O
/// threads are never counted against the render-side budget.
unsigned computeWorkers(const ThreadBudget& b, unsigned renderSpawned);
/// The full tally for the census summary (instance() logs it at info level on every boot).
std::string threadTally(const ThreadBudget& b);
/// Automatic settings never warn. Non-empty only when an explicit override (--cores /
/// LITHO_CORES via b.source, or an explicit --render-threads giving renderSpawned) pushes
/// compute workers past both the hardware cores (b.hwRaw) and what automatic settings
/// would plan on that hardware. instance() / renderBands() log it at most once each.
std::string oversubscriptionWarning(const ThreadBudget& b, bool renderExplicit = false, unsigned renderSpawned = 0);
/// Invariant after the pool merges (t3353u): live Class B worker records never exceed the
/// budget pool. Empty when it holds; cJobPool::shared() logs a WARN otherwise.
std::string poolInvariantViolation(size_t livePooled, const ThreadBudget& b);

/// JD t3349u item 4: retire() with this tick records the last committed Timing Handshake
/// tick (cThreadCensus::lastCommittedTick()) instead of 0.
constexpr uint64_t kLastCommittedTick = UINT64_MAX;

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

/// Threads in this process per the OS (0 if unsupported). Used by the P2 leak test.
size_t osThreadCount();

/// Process-wide census. budget() is fixed after the first call. Records are append-only.
class cThreadCensus {
 public:
  static cThreadCensus& instance();

  /// Census for an explicit core count (tests). instance() resolves CLI > LITHO_CORES > hardware.
  explicit cThreadCensus(unsigned hwRaw, CoresSource source = CoresSource::Hardware);

  /// CLI core override (ruling 5). Must be called before the first instance(); returns
  /// false (and changes nothing) once the census has booted.
  static bool setCliCores(unsigned cores);

  /// JD t3349u item 4: the Timing Handshake notes each committed tick here, so shutdown
  /// records carry the last committed tick rather than 0.
  static void noteCommittedTick(uint64_t tick);
  static uint64_t lastCommittedTick();

  /// Band count for a renderer request against this census (ruling 5).
  /// An explicit req that oversubscribes (see oversubscriptionWarning) logs one WARN.
  unsigned renderBands(unsigned req) const;

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
  /// P2: one thread joined by its owner -> RETURNED, reason EnginePowerDown. Idempotent.
  void retire(uint32_t id, uint64_t tick);
  /// P2: records not yet RETURNED (all classes, or one class).
  size_t live() const;
  size_t live(ThreadClass cls) const;

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
  mutable litholattice::cAtomicGate m_;  ///< guards slots_ growth and telemetry; hot path is the atomic word
  std::vector<std::unique_ptr<Slot>> slots_;
};

/// P2: the records one owner holds for the threads it spawned. add() after the spawn,
/// retire(i) / retireAll() after the join. The destructor retires anything left, so a
/// record never outlives its owner. Uses cThreadCensus::instance().
class cThreadRecordSet {
 public:
  cThreadRecordSet() = default;
  cThreadRecordSet(const cThreadRecordSet&) = delete;
  cThreadRecordSet& operator=(const cThreadRecordSet&) = delete;
  ~cThreadRecordSet() { retireAll(); }
  void add(ThreadClass cls, ThreadRole role, uint16_t roleIndex, const char* name);
  void retire(size_t i, uint64_t tick = kLastCommittedTick);
  void retireAll(uint64_t tick = kLastCommittedTick);
  size_t size() const { return ids_.size(); }

 private:
  std::vector<uint32_t> ids_;
  std::vector<uint8_t> retired_;
};

}  // namespace litholattice
