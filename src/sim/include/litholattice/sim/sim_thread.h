// Cortana 2026-10-15 0825: CHUNK-STALL holdBetweenTicks
// Tick scheduler / simulation thread: owns the active dimension's ChunkMap while running.
// Fixed-timestep world tick (32/64/128/256 TPS, runtime-selectable), clock domains the game
// declares (each at its own power-of-two rate), player edits, raycast for the block highlight,
// the game's IWorldSim, and remesh snapshots. No GL, no render types: remeshing is
// requested through the SubmitMesh callback with an immutable MeshInput.
#pragma once
#include "litholattice/core/thread_census.h"  // Thread Handshake P2
#include "litholattice/core/gate_lock.h"
#include "litholattice/core/lock_timing.h"
#include "litholattice/core/threeway.h"  // LL_THREEWAY (Proposed by JD; co-implemented by JD and Cortana)
#include <chrono>
#include <array>
#include <atomic>
#include <cassert>  // Cortana 2026-10-15 1658: F-2
#include <cstdint>
#include <cstdio>  // LL_SIM_STALL_MS parse (Proposed by Herominde; implemented by Cortana)
#include <functional>
#include "litholattice/core/atomic_gate.h"
#include "litholattice/core/block_gate.h"
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "litholattice/core/fail_fast.h"
#include "litholattice/core/threading.h"
#include "litholattice/core/time.h"
#include "litholattice/sim/clock_domain.h"
#include "litholattice/sim/random_ticks.h"
#include "litholattice/sim/timing_handshake.h"
#include "litholattice/sim/world_simulation.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/mesh_input.h"

namespace litholattice {

class cControlStream;   // control_stream.h — the serialized control-event stream
struct ControlEvent;   // control_stream.h — one stamped event (the POST scratch holds these)

constexpr int kTickModes[] = {32, 64, 128, 256};  // world TPS modes (powers of two)
constexpr int kDefaultTps = 32;

struct RayInput { float ox = 0, oy = 0, oz = 0, dx = 0, dy = 0, dz = 1; };  // render -> sim

// cPlayer actions (render -> sim). Break/Place act on the current ray hit (reach kReach);
// Use carries an explicit cell (right-click on an interactive block); Command is game-defined
// and goes to IWorldSim::command.
// Cortana 2026-10-15 2215 [ENGINE]: kReach is the sim's MAXIMUM edit reach (creative 16, JD D5);
// games gate tighter per mode (LithoKraft survival 5) with SimStats::hitDist before sending edits.
constexpr float kReach = 16.f, kLookRange = 64.f;
/// 2026-10-25 (JD): would a solid block at cell (x, y, z) overlap the player whose eye is at
/// (ox, oy, oz)? Player box: 0.6 wide, feet 1.62 below the eye, head 0.18 above (conservative
/// for sneak / crawl). Such a place is refused, never resolved by pushing the player.
inline bool placeOverlapsPlayer(int x, int y, int z, float ox, float oy, float oz) {
  constexpr float kHalfW = 0.3f, kBelow = 1.62f, kAbove = 0.18f, kEps = 1e-3f;
  return float(x) < ox + kHalfW - kEps && float(x + 1) > ox - kHalfW + kEps &&
         float(z) < oz + kHalfW - kEps && float(z + 1) > oz - kHalfW + kEps &&
         float(y) < oy + kAbove - kEps && float(y + 1) > oy - kBelow + kEps;
}
struct EditCmd {
  enum class Kind : uint8_t { Break, Place, Use, Command } kind = Kind::Break;
  BlockId block = 0; uint8_t facing = 0;   // Place
  int x = 0, y = 0, z = 0;                 // Use (and copied into Command)
  // Place: x == kPlaceReceipt asks the sim for an outcome receipt (applied or
  // rejected), drained in order by cSimThread::drainPlaceReceipts (2026-10-16:
  // the game refunds the item a rejected place consumed). Command: x == kPlaceReceipt
  // receipts whether the command's target cell (command.x/y/z) changed (walk-bridging).
  SimCommand command{};                    // Command
  /// Sim-stamped target world tick (set on drain). Edit application is a pure
  /// function of tick index: the edit lands on this tick, whenever it runs.
  uint64_t tick = 0;
};

/// Historical sentinel for the paused path's apply-everything call. Control
/// serialization made it unnecessary (paused application is stamp-driven:
/// events stamped at drain carry the current tick and are due immediately);
/// retained because the value is part of this header's vocabulary.
inline constexpr uint64_t kApplyAllEdits = UINT64_MAX;
inline constexpr int kPlaceReceipt = 1;

constexpr int kMaxClockDomains = 4;

/// Everything cSimThread needs besides the world and the game's simulation.
struct SimConfig {
  int tickRate = kDefaultTps;      ///< world TPS (one of kTickModes)
  int radiusChunks = 8;            ///< tick radius around the player
  bool roundArea = false;          ///< F56-7: round (disc) tick area; default square (hash scenarios unchanged)
  bool diamondArea = false;        ///< diamond (Manhattan) tick area (JD 2026-10-23); wins over roundArea
  uint32_t epoch = 0;              ///< dimension load generation (stamped on mesh snapshots)
  BlockId belowWorldFill = kAir;   ///< what the mesher sees below y = 0 (solid hides bottom faces)
  int randomTicksPerSection = 8;   ///< per 16^3 section per world tick (per dimension; 0 = off)
  std::vector<ClockDomain> domains;  ///< up to kMaxClockDomains, power-of-two rates
  // Timing Handshake (hs-design §5): DEFAULT ON since 2026-10-14 (JD's
  // promotion directive — the handshake is the standard timing path). An
  // unconfigured cSimThread constructs the authority; legacy mode is the
  // explicit opt-out (timingHandshake = false), which constructs no
  // authority and runs the legacy path byte-for-byte.
  bool timingHandshake = true;
  int handshakePhasesPerCycle = 4;   ///< phases per cycle (Signal needs >= 4)
  int handshakeWorkerThreads = 0;    ///< 0 = authority auto
  int handshakeWatchdogMs = 0;       ///< 0 = authority default (250 ms)
  bool handshakePureSpin = false;    ///< D9 A/B flag: true = pull workers pure-spin instead of the hybrid poll (review A-L9)
};

struct SimStats {              // sim -> render, every wake-up (lock-free triple buffer)
  uint64_t worldTick = 0;
  int targetTps = 0, masterHz = 0;
  float tps = 0.f, tickMs = 0.f, tickMaxMs = 0.f, budgetPct = 0.f;  // tick ms: avg/max over last 1 s
  uint64_t droppedTicks = 0;
  uint32_t ticksLastPass = 0;              // world ticks executed in the most recent loop pass (S6, 2026-10-28)
  uint32_t active = 0;                     // IWorldSim::activeCount()
  uint64_t randomTicks = 0;                // opted-in cells random-ticked so far
  struct Domain { int hz = 0; float achieved = 0.f; bool enabled = false; };
  std::array<Domain, kMaxClockDomains> domains{};  // in SimConfig::domains order
  int domainCount = 0;
  uint64_t hsOverrunPhases = 0;   // Timing Handshake stats (0 in legacy mode)
  uint32_t hsOffenders = 0;       // components flagged repeat overrun offenders
  bool hsFrozen = false;          // watchdog freeze latch
  bool hit = false, inReach = false; float hitDist = 0;  // looked-at block (up to kLookRange)
  int hx = 0, hy = 0, hz = 0; BlockId hitBlock = 0; CellState hitState = 0;
  char hitInfo[256] = {};  ///< look-at inspector line from IWorldSim::inspect
  /// 2026-10-25 (JD, Post viewer): per-cell text of the looked-at cell (or of the cell above when
  /// that is the same block, e.g. a two-high post's panel); empty when none. Up to 256 chars.
  char hitText[260] = {};
};

class cSimThread {
 public:
  using SubmitMesh = std::function<void(std::shared_ptr<MeshInput>)>;
  /// `simulation` is used on the sim thread only; `blocks` must outlive the thread.
  cSimThread(ChunkMap& chunks, const cBlockReg& blocks, std::unique_ptr<IWorldSim> simulation,
            SimConfig config, SubmitMesh submit);
  ~cSimThread();
  void start();
  // Stops the sim thread, then the handshake authority. Returns false in
  // handshake mode when the authority's no-detach stop() failed: a pull
  // worker is still outstanding (authority poisoned), the sim must NOT be
  // treated as safely stopped for save-completion or dimension switch,
  // and start() will refuse until a retried stop() succeeds. Callers that
  // ignore the result keep the old behaviour only in legacy mode, where
  // stop() always returns true. (P0a / review C2 — see timing_handshake.h.)
  bool stop();
  /// True while a handshake pull worker is outstanding after a failed
  /// stop() (poisoned; see stop()).
  bool handshakeWorkersOutstanding() const { return handshake_ && handshake_->workersOutstanding(); }
  // Render thread -> sim. Control inputs cross the serialized control
  // stream (control_stream.h): the sim thread stamps them at the PRE phase
  // and applies them at the POST phase, in (tick, seq) order.
  /// Sets the player chunk (the tick area). A stream event now, not an
  /// atomic: the area in effect for tick T is the last PlayerChunk event
  /// stamped <= T, so the tick area is a function of the stream.
  void setPlayerChunk(int cx, int cz);
  void setTickRate(int tps) { validateTickRate(tps); requestedTps_.store(tps); notifyWake(); }
  /// Paused: no ticks run (edits / raycast still apply); unpausing does not catch up.
  void setPaused(bool p) { paused_.store(p); notifyWake(); }
  bool paused() const { return paused_.load(); }
  /// Step N WORLD ticks while paused (creative-mode tick debugger).
  /// If not paused, pauses first. Each counted step is one full world tick
  /// run through the SAME step body as the live path (clock domains, tick-
  /// quantized edits, sim tick, randomTick AND runRandomTicks, window/stat
  /// accounting): with faster clock domains configured the stepper advances
  /// as many master steps as it takes until N world ticks have run, so
  /// stepTicks(n) advances worldTick by exactly n (review CH-H5).
  void stepTicks(uint64_t n) { stepRequested_.fetch_add(n); if (!paused()) setPaused(true); notifyWake(); }
  /// Run `fn` between ticks: pauses the sim, waits for any in-flight tick to
  /// finish (the block mutex is held across the sim's whole mutation window),
  /// runs fn with the mutex held, then restores the pause state.
  /// Hot-swap refresh uses this: pause one tick, swap block definitions,
  /// resume — no torn reads mid-tick.
  void runBetweenTicks(std::function<void()> fn);
  /// 2026-10-31 (JD, Save World): lock-free one-slot tick-boundary job. The tick thread runs
  /// `fn` itself at the start of its next mutation window: between two ticks, with blockMutex_
  /// already held BY THE TICK THREAD, so no other thread stops, pauses or waits on the sim.
  /// Publish is one compare-exchange on an atomic pointer; the tick thread claims it with one
  /// exchange (no lock on either side). Deferred while the handshake is frozen or a pull worker
  /// is outstanding. Returns false when a job is already pending or the sim is not running.
  /// The sim never changes state for it: fn must only read the world (snapshots).
  bool postBoundaryJob(std::function<void()> fn);
  /// Drops a still-pending boundary job (it never runs). True when one was dropped; false when
  /// none was pending (it already ran or is running on the tick thread).
  bool cancelBoundaryJob();
  /// Stream hand-off (2026-10-15): lock the sim between iterations WITHOUT
  /// pausing or stopping it, so the game thread can insert/erase chunk
  /// columns. Returns false (nothing held) when the timing handshake is
  /// frozen or has an outstanding worker; callers then fall back to stop().
  bool holdBetweenTicks(::litholattice::cGateLock<cBlockGate>& hold);
  /// 2026-10-25 (JD hitch): non-blocking form for the frame thread. Returns false with *busy = true
  /// when the sim is mid-tick (blockMutex_ taken) instead of waiting the tick out; false with
  /// *busy = false is the same refusal as holdBetweenTicks (frozen / workers outstanding).
  bool tryHoldBetweenTicks(::litholattice::cGateLock<cBlockGate>& hold, bool* busy);
  /// Ticks stepped so far (for test verification).
  uint64_t ticksStepped() const { return ticksStepped_.load(std::memory_order_acquire); }
  /// Flips clock domain `index` on / off (SimConfig::domains order).
  void toggleDomain(int index) { if (index >= 0 && index < kMaxClockDomains) { domainToggles_.fetch_xor(1u << index); notifyWake(); } }
  /// Aim ray. Two consumers, two paths (the alpha-presentation doctrine):
  /// the HUD look-at highlight reads the raw triple-buffer slot
  /// (presentation-only, latest wins), while edit resolution reads the
  /// STAMPED ray from the control stream — so a Break/Place resolves
  /// against the aim in effect on its own tick, and a replayed stream
  /// resolves identically.
  void setRay(const RayInput& r);
  /// Sim loop iterations started so far (each drains edits, then ticks). Waiting for two more than
  /// a reading guarantees one whole iteration ran after it: tests wait on progress, not wall time.
  uint64_t loops() const { return loops_.load(std::memory_order_acquire); }
  /// Measure-only windowed determinism probe (LL_HASH_AT_TICK; Proposed by JD; co-implemented by JD and Cortana): called on the sim thread right
  /// after each completed world tick, inside the mutation window. Set before start(); empty = never called.
  void setTickHook(std::function<void(uint64_t)> fn) { tickHook_ = std::move(fn); }
  /// Cortana 2026-10-26 (Archie camera-jump decision): runs on the sim thread
  /// at the end of every COMPLETED world tick, inside the tick's block-mutex
  /// window (getBlock from the hook is the sim thread's own recursive hold).
  /// Set before start(); the game steps the player body here.
  void setPlayerHook(std::function<void(uint64_t)> fn) { playerHook_ = std::move(fn); }
  /// True from start() until stop() returns (the thread is joined after).
  bool running() const { return running_.load(std::memory_order_acquire); }
  /// World TPS in force for the current tick. SIM THREAD ONLY (the player hook).
  int simThreadWorldTps() const { return cfgWorldTps_; }
  /// LL_HANDOFF_COUNT probe (measure only): handoffs fired, reads and inserts served by them so far.
  void handoffStats(uint64_t& handoffs, uint64_t& reads, uint64_t& inserts) const { handoffs = handoffs_.load(); reads = servedReads_.load(); inserts = servedInserts_.load(); }
  /// Single-block read for threads other than the sim thread (game thread entity
  /// collision, MCP world_get_block). Race-free: takes blockMutex_, which the sim
  /// thread holds across its whole mutation window (edit drain + every tick of the
  /// iteration), so no ChunkMap write can interleave with this read. May block for
  /// up to one catch-up burst; fine for a handful of queries per frame, not for
  /// bulk scans.
  BlockId getBlock(int x, int y, int z) {
    PointRead pr(threewayYield_ ? &pointReaders_ : nullptr, threewayYield_ ? &readWaiting_ : nullptr);  // LL_THREEWAY (Proposed by JD; co-implemented by JD and Cortana)
    locktiming::TimedGuard l(blockMutex_, locktiming::kBlock);  // 0.0.1.60 timing (diagnostic)
    pr.done();
    return chunks_.get(x, y, z);
  }
  /// 2026-10-25 (JD hitch): non-blocking getBlock for the frame thread; nullopt while the sim is mid-tick.
  std::optional<BlockId> tryGetBlock(int x, int y, int z) {
    ::litholattice::cGateLock<cBlockGate> l(blockMutex_, ::litholattice::kTryToLock);
    if (!l.owns_lock()) return std::nullopt;
    return chunks_.get(x, y, z);
  }
  /// Block id and cell state in one locked read (Highway Ramp player collision, JD 2026-10-16).
  std::pair<BlockId, CellState> getBlockAndState(int x, int y, int z) {
    PointRead pr(threewayYield_ ? &pointReaders_ : nullptr, threewayYield_ ? &readWaiting_ : nullptr);  // LL_THREEWAY (Proposed by JD; co-implemented by JD and Cortana)
    locktiming::TimedGuard l(blockMutex_, locktiming::kBlock);  // 0.0.1.60 timing (diagnostic)
    pr.done();
    return {chunks_.get(x, y, z), chunks_.getState(x, y, z)};
  }
  /// Blocks until `n` more iterations have started, or `timeout` passes (false then).
  bool waitLoops(uint64_t n, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) const;
  /// A player edit (Local source). Crosses the control stream: stamped at
  /// the next PRE phase with the tick it lands on, applied in (tick, seq)
  /// order at that tick's POST phase. Producers that know a different
  /// source (MCP pilots, scripts) submit via controlStream() directly.
  void edit(EditCmd c);
  /// Outcomes of receipted Place edits in application order (1 = applied,
  /// 0 = rejected); clears the queue. Bounded: at most 4096 are kept.
  std::vector<uint8_t> drainPlaceReceipts();
  /// The serialized control stream (producers submit; IDemoRecord taps
  /// the record; tests inject stamped streams). Owned by this thread.
  cControlStream& controlStream() { return *control_; }
  const cControlStream& controlStream() const { return *control_; }
  /// Re-snapshots every loaded sub-chunk for meshing at the next tick (textures were reloaded).
  void requestRemeshAll() { remeshAll_.store(true); notifyWake(); }
  /// PlayJ fix 3: when false, flushDirty keeps dirty flags but skips pinForMeshing / submit (null renderer / headless benches).
  void setMeshConsumer(bool on) { meshConsumes_ = on; }
  bool meshConsumer() const { return meshConsumes_; }
  /// Columns whose cells changed (player edits, simulation ticks, random ticks, commands) since
  /// the last call: what a partial save must write. Clears the set. Call only while stopped.
  std::unordered_set<uint64_t> takeChangedColumns() { std::unordered_set<uint64_t> out; out.swap(changedCols_); return out; }
  // Cortana 2026-10-15 1658: F-2 [ENGINE] the triple buffer has exactly one reader.
  // Debug builds bind the first calling thread and assert every later call comes
  // from it; other threads (MCP handlers) must use a game-side copy.
  const SimStats& stats() {
#ifndef NDEBUG
    std::thread::id expect{};
    const std::thread::id me = std::this_thread::get_id();
    if (!statsReader_.compare_exchange_strong(expect, me, std::memory_order_relaxed))
      assert(expect == me && "cSimThread::stats(): single reader thread only (F-2)");
#endif
    return stats_.read();
  }  // render thread only
  /// Game thread: simulation events (sounds, ...) since the last call (appended to `out`).
  void drainEvents(std::vector<SimEvent>& out) { events_.drainInto(out); }
  /// The game's simulation. Touch it only while the thread is stopped (before start() or after
  /// stop()), e.g. to save or restore its state; start() may be called again afterwards.
  IWorldSim& simulation() { return *sim_; }
  /// Timing Handshake authority (null in legacy mode). Registration is
  /// pre-start only; test harnesses may add components before start().
  cTimingHandshakeAuthority* handshakeAuthority() { return handshake_.get(); }
  bool handshakeFrozen() const { return handshake_ && handshake_->frozen(); }
  /// Recovery from a watchdog freeze (the tick_unfreeze path in handshake
  /// mode): clears the authority latch; the interrupted cycle is abandoned
  /// and the simulation has already forced a full pass for the next tick.
  void resetHandshakeAfterFreeze() {
    if (handshake_) handshake_->resetAfterFreeze();
    freezeReported_.store(false);
    stepRefusalLogged_.store(false);
  }
  /// S6 (2026-10-28, Archie ruling; JD direction): zero the accumulated
  /// wall-clock time debt (acc_) at a presentation boundary — the game
  /// calls this on the exact frame of the MENU -> PLAY transition, so
  /// time burned in RESOURCE_LOAD / menu (cores saturated, this thread
  /// starved) is discarded instead of landing as a catch-up burst plus
  /// a dropped-tick spike on the first play frames. The sim thread
  /// consumes the request at its accumulator: acc_ is zeroed and the
  /// pacing clock re-based BEFORE the lag clamp can book the debt as
  /// drops; the pre-reset debt and the last pass's tick count are
  /// logged. Presentation-boundary only — no per-tick math changes,
  /// and the cumulative dropped_ stat is NOT rewound.
  void resetTimeDebt() { debtResetReq_.store(true, std::memory_order_release); notifyWake(); }

 private:
  void run();
  /// POST phase of the control coupling: extract every stream event
  /// stamped for `tick` or earlier and apply them in (tick, seq) order —
  /// Ray events move the stamped aim, PlayerChunk events move the tick
  /// area, Edit events run the edit application below against the stamped
  /// ray. Events not yet due stay pending. Returns true if any EDIT was
  /// due (the remesh-at-once signal, as the old applyEdits returned).
  bool applyControlEvents(uint64_t tick);
  /// One edit against the current stamped-ray cast (the pre-serialization
  /// applyEdits loop body, one event at a time so stream order — including
  /// Ray events between edits — is honored exactly). Returns true when the
  /// edit changed the world (the caller re-casts before the next edit).
  bool applyOneEdit(EditCmd& e, const std::function<void(int, int, int)>& dirty);
  bool applyPlace(EditCmd& e);
  cAtomicGate receiptM_;  // PlayL: was ::litholattice::cAtomicGate
  std::vector<uint8_t> receipts_;
  /// Look-at raycast for the HUD highlight only (no edit application).
  void castRay(const RayInput& ray);
  /// simFailFast unless tps is a supported tick mode (kTickModes ∪ {512},
  /// review A-L1). Domain rates are likewise fail-fasted in the ctor when
  /// non-pow2 or above kMaxDomainHz.
  static void validateTickRate(int tps);
  void handleHandshakeFreeze();  // watchdog: pause + report (never simFailFast)
  // Paused-loop parking (review A-M7 / PlayL): bumps the wake epoch.
  // Every external change a parked paused loop must react to (pause toggle,
  // step request, edit, ray, rate/domain change, remesh request, stop) goes
  // through here. The loop spin/polls the epoch (no CV; threads never wait
  // on each other).
  void notifyWake() {
    // PlayL JD: no CV wait/notify; bump epoch so a paused loop's spin/poll sees it.
    wakeEpoch_.fetch_add(1, std::memory_order_acq_rel);
  }
  void markDirty(int x, int y, int z, bool activeSignal = false);
  void popAttached(int x, int y, int z);
  void simChanged(int x, int y, int z);
  void flushDirty(bool forceInView = false);  // Cortana 2026-11-06: edit flush skips view cull

  // ---- Timing Handshake (null in legacy mode) ----
  std::unique_ptr<cTimingHandshakeAuthority> handshake_;
  bool handshakeMode_ = false;
  std::atomic<bool> freezeReported_{false};  // the current freeze was reported
  std::atomic<bool> stepRefusalLogged_{false};  // frozen step refusal logged once
  uint64_t hsMasterStep_ = 0;     // sim thread: current master step (domain cadence)
  int hsMasterHz_ = 1;            // sim thread: current master rate (domain cadence)
  bool hsCountWindow_ = false;    // sim thread: count domain windowTicks this step
  std::vector<std::string> hsDomainNames_;  // owned names for domain components

  ChunkMap& chunks_;
  // getBlock() <-> sim-thread mutation-window protocol: the sim thread holds this
  // across the edit drain + every tick of each loop iteration; getBlock() takes it
  // for the read. Uncontended on the sim thread; getBlock() may wait one burst.
  // Recursive: runBetweenTicks holds it across fn(), and fn() may legitimately
  // call getBlock() (which takes the same gate). A plain non-reentrant gate deadlocks there.
  // Archie PlayI: cBlockGate (atomics), not std::recursive_mutex.
  // PlayJ fix 7 note: tick mutation window + getBlock/holdBetweenTicks still need this ownership gate
  // (phase boundary between sim/main/stream). remesh/flush unlocks it when CoW columns are on
  // (pin reads snapshots). Cannot remove for non-CoW pin or mid-tick getBlock without a new
  // ownership/phase handoff for live column pointers.
  cBlockGate blockMutex_;
  // Three-way split timing handshake (LL_THREEWAY=1, default OFF). Proposed by JD; co-implemented by JD and Cortana.
  // Point readers (getBlock / getBlockAndState) announce themselves; between whole ticks of a catch-up
  // burst the sim hands blockMutex_ over for their short read instead of making them wait out the
  // whole burst. A reader never sees a half-applied tick. Off: identical to the old path.
  const bool threewayYield_ = ::litholattice::threeway::enabled();
  std::atomic<int> pointReaders_{0};
  // LL_HANDOFF_COUNT=1 (measure only; 311): counts between-tick handoffs and the reads / inserts
  // that were waiting when each one fired. Off: nothing is counted.
  const bool handoffCount_ = [] { const char* e = std::getenv("LL_HANDOFF_COUNT"); return e && e[0] == '1'; }();
  std::atomic<int> readWaiting_{0}, insertWaiting_{0};
  std::atomic<uint64_t> handoffs_{0}, servedReads_{0}, servedInserts_{0};
  std::function<void(uint64_t)> tickHook_;
  struct BoundaryJob { std::function<void()> fn; };
  std::atomic<BoundaryJob*> boundaryJob_{nullptr};  ///< see postBoundaryJob (single slot, lock-free)
  std::function<void(uint64_t)> playerHook_;  // player body step (see setPlayerHook)  // LL_HASH_AT_TICK probe (measure only)
  // LL_SIM_STALL_MS=<ms>@<tick>[x<n>] (measure only, default OFF; Proposed by Herominde; implemented by Cortana). At world tick <tick> (then every
  // 128 ticks, n times) the sim thread sleeps <ms> while holding blockMutex_, so streaming inserts and point reads queue up
  // and the catch-up burst that follows serves them. Logs the world tick every insert lands on near the stalls. No sim logic changes.
  struct StallCfg { int ms = 0; unsigned long long tick = 0; int count = 0; };
  static StallCfg parseStall() {
    StallCfg c; const char* e = std::getenv("LL_SIM_STALL_MS"); if (!e) return c;
    int ms = 0, n = 1; unsigned long long t = 0;
    if (std::sscanf(e, "%d@%llux%d", &ms, &t, &n) >= 2 && ms > 0) { c.ms = ms; c.tick = t; c.count = n < 1 ? 1 : n; }
    return c;
  }
  const StallCfg stall_ = parseStall();
  int stallsDone_ = 0;          // sim thread, under blockMutex_
  bool inBurst_ = false;        // sim thread, under blockMutex_ (still true across a handoffYield)
  bool stallLogWindow() const { return stall_.ms > 0 && stallsDone_ > 0 && worldTickN_ <= stall_.tick + 128ull * static_cast<unsigned long long>(stall_.count) + 128ull; }
  struct PointRead {
    std::atomic<int>* c; std::atomic<int>* k;  // k: per-kind waiting count (reads / inserts) for LL_HANDOFF_COUNT
    explicit PointRead(std::atomic<int>* p, std::atomic<int>* kind = nullptr) : c(p), k(kind) { if (c) c->fetch_add(1, std::memory_order_acq_rel); if (k) k->fetch_add(1, std::memory_order_acq_rel); }
    void done() { if (c) { c->fetch_sub(1, std::memory_order_acq_rel); c = nullptr; } if (k) { k->fetch_sub(1, std::memory_order_acq_rel); k = nullptr; } }
    ~PointRead() { done(); }
  };
  void handoffYield() {  // sim thread only, between ticks, holding blockMutex_ exactly once
    if (handoffCount_) { handoffs_.fetch_add(1, std::memory_order_relaxed); servedReads_.fetch_add(uint64_t(readWaiting_.load(std::memory_order_acquire)), std::memory_order_relaxed); servedInserts_.fetch_add(uint64_t(insertWaiting_.load(std::memory_order_acquire)), std::memory_order_relaxed); }  // LL_HANDOFF_COUNT
    locktiming::setBlockHolder(locktiming::kHolderNone);
    blockMutex_.unlock();
    const auto t0 = std::chrono::steady_clock::now();
    while (pointReaders_.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1))
      std::this_thread::yield();
    blockMutex_.lock();
    locktiming::setBlockHolder(locktiming::kHolderSimTick);
  }
  const cBlockReg& blocks_;
  std::unique_ptr<IWorldSim> sim_;
  const int radius_;
  const bool round_;  ///< F56-7
  const bool diamond_;  ///< diamond (Manhattan) tick area (JD 2026-10-23)
  int randomPerSection_ = 8;
  std::vector<uint8_t> randomTickable_;
  uint64_t randomTicks_ = 0;
  std::atomic<uint64_t> loops_{0};
  const uint32_t epoch_;
  const BlockId belowWorld_;
  SubmitMesh submit_;
  std::atomic<bool> running_{false};
  std::atomic<int> requestedTps_;
  std::atomic<uint32_t> domainToggles_{0};
  std::atomic<bool> remeshAll_{false};
  std::thread thread_;
  cThreadRecordSet threadRecord_;  // Thread Handshake P2: the sim clock (Class A)
  TripleBuffer<SimStats> stats_;
  std::atomic<std::thread::id> statsReader_{};  // Cortana 2026-10-15 1658: F-2 (checked in Debug only)
  float rayEye_[3] = {0.f, -1e9f, 0.f};  ///< 2026-10-25: eye of the last cast ray (placement overlap check)
  float rayDir_[3] = {0.f, 0.f, 1.f};    ///< PlayJ: look direction for remesh frustum cull
  TripleBuffer<RayInput> ray_;  // presentation only (HUD look-at); edit resolution uses stampedRay_
  // The serialized control stream: every sim-affecting control input
  // crosses here (edits, the stamped ray, the player chunk). Constructed
  // with the cSimThread; producers submit from any thread; the sim thread
  // alone stamps (PRE) and extracts (POST).
  std::unique_ptr<cControlStream> control_;
  std::unordered_set<uint64_t> changedCols_;  // sim thread (or caller while stopped)
  cHandoffQueue<SimEvent> events_;       // sim -> game
  std::vector<SimEvent> eventBuf_;
  void publishEvents();

  // ---- sim-thread-owned state ----
  std::vector<ClockDomain> domains_;
  std::vector<bool> domainInitial_;
  std::unordered_set<int64_t> dirtySubs_;
  std::unordered_set<int64_t> signalDirtySubs_;  // changing signal appearances bypass mesh view cull
  std::vector<ControlEvent> controlDue_;  // POST-phase extraction scratch (stream order)
  // Stamped control state (sim thread): the aim and tick area in effect
  // for the tick being run — pure functions of the applied stream prefix.
  RayInput stampedRay_{};
  uint64_t areaChunkKey_ = 0;  // ChunkKey::make(0, 0), as the old atomic default
  // Pooled MeshInput buffers: the pool is shared with the submit() deleter so it
  // outlives any in-flight snapshot (a cSimThread may die while mesh workers still
  // hold snapshots). Cap bounds the retained memory (~64 x 24 KB).
  // PlayI (JD, lock-free): fixed slot array, ownership handed off by atomic exchange / CAS (no gate).
  struct MeshInputPool {
    static constexpr int kSlots = 64;
    std::atomic<MeshInput*> slot[kSlots] = {};
    std::unique_ptr<MeshInput> take() { for (auto& s : slot) if (MeshInput* p = s.exchange(nullptr, std::memory_order_acquire)) return std::unique_ptr<MeshInput>(p); return nullptr; }
    void give(std::unique_ptr<MeshInput> u) { MeshInput* p = u.get(); for (auto& s : slot) { MeshInput* e = nullptr; if (s.compare_exchange_strong(e, p, std::memory_order_release, std::memory_order_relaxed)) { u.release(); return; } } }  // full: u frees it
    ~MeshInputPool() { for (auto& s : slot) delete s.exchange(nullptr); }
  };
  std::shared_ptr<MeshInputPool> meshPool_ = std::make_shared<MeshInputPool>();
  RandomTickCache randomTickCache_;  // ticking column pointers (opt: skip hash lookups)
  uint64_t meshSeq_ = 1ull << 32;  // above the initial-load sequence numbers
  std::unordered_map<uint64_t, uint64_t> flushMasks_;               // PlayI: flushDirty scratch, reused (no per-flush alloc)
  std::vector<std::pair<uint64_t, uint64_t>> flushOrdered_;         // PlayI: flushDirty scratch, reused
  std::vector<int64_t> flushWork_;                                 // PlayJ: in-view dirty keys this flush
  std::vector<int64_t> flushHold_;                                 // PlayJ: out-of-view dirty keys retained
  bool meshConsumes_ = true;                                       // PlayJ fix 3: pin/submit when a mesh consumer exists
  uint64_t flushBatch_ = 0;  // Cortana 2026-10-15 1350: phase-boundary publish batch id

  // ---- persistent tick state (review CH-H8) — sim thread only ----
  // Initialised ONCE at construction and deliberately NOT reset by
  // stop()/start(): masterStep_/worldTickN_/dropped_ and the accumulator /
  // window state survive the routine stop/start cycles of every save and
  // stream pump, so keyed random streams (seeded by world tick) never
  // replay, edit stamps never regress, and SimStats.worldTick never goes
  // backwards across a restart. A freshly constructed cSimThread still
  // starts at 0 — only restarts changed. (Wall-clock pacing — the `last`
  // timestamp in run() — stays a run() local, re-based on each start.)
  uint64_t masterStep_ = 0, worldTickN_ = 0, dropped_ = 0;
  uint32_t lastPassTicks_ = 0;  // S6: world ticks executed in the last loop pass (published as SimStats::ticksLastPass)
  std::chrono::nanoseconds acc_{0};
  Clock::time_point windowStart_{};  // zero = unset; run() bases it on entry
  uint64_t windowWorldTicks_ = 0;
  double windowWorldNs_ = 0, windowAllNs_ = 0, windowWorldMaxNs_ = 0;
  float statTps_ = 0, statTickMs_ = 0, statTickMaxMs_ = 0, statBudget_ = 0;
  int cfgWorldTps_ = 0, cfgMasterHz_ = 0, cfgWorldDiv_ = 1;
  std::chrono::nanoseconds stepDur_{0};
  uint32_t togglesSeen_ = 0;
  bool configured_ = false;
  // Shared step-body scratch (CH-H5): set per loop iteration before the
  // step body runs; the body reports edit/raycast work back through these.
  bool stepEditsApplied_ = false, stepRaycastDone_ = false;

  // ---- paused-loop parking (review A-M7 / PlayL: epoch spin/poll, no CV) ----
  std::atomic<uint64_t> wakeEpoch_{0};  // bumped by notifyWake()
  bool hit_ = false; float hitDist_ = 0; int hit_xyz_[3]{}, prev_xyz_[3]{};
  // Look-at inspector cache: sim_->inspect is re-run only when the looked-at cell
  // changes (formatting it every loop iteration was pure churn).
  std::string hitInfoCache_;
  int hitInfoXyz_[3]{};
  bool hitInfoValid_ = false;
 std::atomic<bool> paused_{false};
  std::atomic<bool> debtResetReq_{false};  ///< S6: resetTimeDebt() request, consumed by the sim thread at its accumulator
  std::atomic<uint64_t> stepRequested_{0};  ///< pending single-step ticks (tick debugger)
  std::atomic<uint64_t> ticksStepped_{0};   ///< ticks completed via stepTicks
};


}  // namespace litholattice
