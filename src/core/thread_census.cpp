// Thread Handshake P1: see thread_census.h. Record-only; nothing takes its counts yet.
#include "litholattice/core/thread_census.h"
#include <cstring>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include "litholattice/core/log.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include "litholattice/core/atomic_gate.h"
#endif

namespace litholattice {

namespace {
std::atomic<unsigned> g_hwReads{0};
std::atomic<unsigned> g_cliCores{0};
std::atomic<bool> g_booted{false};
std::atomic<uint64_t> g_lastCommittedTick{0};  // JD t3349u item 4

constexpr uint64_t pack(ThreadState s, uint32_t ticket) { return (uint64_t(ticket) << 8) | uint64_t(s); }
constexpr ThreadState stateOf(uint64_t w) { return ThreadState(uint8_t(w & 0xff)); }
constexpr uint32_t ticketOf(uint64_t w) { return uint32_t(w >> 8); }

struct Fnv {
  uint64_t h = 1469598103934665603ULL;
  void byte(uint8_t b) { h ^= b; h *= 1099511628211ULL; }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) byte(uint8_t(v >> (8 * i))); }
};
}  // namespace

CoresResolution resolveCores(unsigned cli, const char* env, unsigned hw) {
  if (cli > 0) return {cli, CoresSource::Cli};
  if (env && *env) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(env, &end, 10);
    if (end && *end == '\0' && v > 0 && v <= 4096) return {unsigned(v), CoresSource::Env};
  }
  return {std::max(1u, hw), CoresSource::Hardware};
}

unsigned remainingAfterPermanentThree(unsigned hw) {
  hw = std::max(1u, hw);
  return hw > 3u ? hw - 3u : 1u;
}

unsigned renderSideBudget(unsigned hw) { return remainingAfterPermanentThree(hw) + 2u; }

// JD 2026-11-14: LK_RENDER_THREADS=1 opts into a four-thread-total shape:
// the permanent three + exactly one render-side thread (1 software raster
// band; mesh/vertex work is caller-runs; reserve 0). Unset keeps the default
// 2+1+0 (and the larger-machine planner). Other values are ignored.
bool compactFourThreadTotal() {
  const char* e = std::getenv("LK_RENDER_THREADS");
  if (!e || !*e) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(e, &end, 10);
  return end && *end == '\0' && v == 1ul;
}

RenderSidePlan planRenderSide(unsigned hw, bool wasm) {
  RenderSidePlan p;
  p.hw = std::max(1u, hw);
  p.remaining = remainingAfterPermanentThree(p.hw);
  p.budget = p.remaining + 2u;
  if (compactFourThreadTotal()) {
    p.swBands = 1u;
    p.meshDedicated = 0u;
    p.reservePool = 0u;
    p.swSpawned = 0u;
    p.swSpawnedDetached = 1u;
    return p;
  }
  const unsigned rasterCap = wasm ? 4u : 16u;
  // Split the render-side budget: about half raster, the rest mesh + a small reserve.
  // Prefer at least 2 software bands when budget >= 3 (TheReefs 4 hw: budget 3 →
  // 2 bands + 1 mesh + reserve 0). 12 hw: budget 11 → 5 bands + 4 mesh + reserve 2.
  unsigned raster = p.budget / 2u;
  if (p.budget >= 3u && raster < 2u) raster = 2u;
  if (raster < 1u) raster = 1u;
  if (raster > rasterCap) raster = rasterCap;
  if (raster >= p.budget && p.budget > 1u) raster = p.budget - 1u;  // keep at least 1 for mesh/reserve
  const unsigned meshRes = p.budget - raster;
  // A leftover of 1 goes to the dedicated mesh worker, not the reserve
  // pool, so TheReefs (budget 3) is 2 bands + 1 mesh + reserve 0. A 0-worker
  // reserve is caller-runs under original handshake (shards already inline).
  if (meshRes >= 6u) p.reservePool = 2u;
  else if (meshRes >= 2u) p.reservePool = 1u;
  else p.reservePool = 0u;
  p.meshDedicated = meshRes - p.reservePool;
  p.swBands = std::max(1u, raster);
  p.swSpawned = p.swBands > 0u ? p.swBands - 1u : 0u;
  p.swSpawnedDetached = p.swBands;
  return p;
}

unsigned autoRenderBands(unsigned cores, bool wasm) {
  return planRenderSide(cores, wasm).swBands;
}

unsigned resolveRenderBands(unsigned req, unsigned cores, bool wasm) {
  cores = std::max(1u, cores);
  const RenderSidePlan p = planRenderSide(cores, wasm);
  if (!req) return p.swBands;
  // Honour an explicit --render-threads, but never take more than the raster slice of
  // the shared render-side budget (or the storage cap).
  const unsigned cap = std::min(kMaxRenderBands, std::max(1u, p.swBands));
  return std::min(req, cap);
}

ThreadBudget computeThreadBudget(unsigned hwRaw, bool wasm) {
  ThreadBudget b;
  b.hwRaw = hwRaw;
  b.hw = std::max(1u, hwRaw);
  b.smallMachine = b.hw <= 4;
  // Handshake shard *jobs* (not OS threads). Unchanged; they stay off the render-side budget.
  b.simShards = std::min(4u, b.hw > 1 ? b.hw - 1 : 1u);

  const RenderSidePlan p = planRenderSide(b.hw, wasm);
  b.remaining = p.remaining;
  b.renderSideBudget = p.budget;
  b.renderBands = p.swBands;
  b.capped = true;  // JD 2026-11-14: the render-side cap always applies

  const char* ly = std::getenv("LL_HS_LAYOUT");
  const char* lf = std::getenv("LL_HS_LOCKFREE");
  const char* tw = std::getenv("LL_THREEWAY");
  const char* ded = std::getenv("LL_DED_LAYOUT");
  const bool orig = ly && std::strcmp(ly, "original") == 0;
  (void)lf;
  const bool threeway = tw && tw[0] == '1';
  const bool dedOn = ded && (ded[0] == '6' || ded[0] == '8');
  b.layoutOriginal = orig;
  // Permanent three: main + tick/sim clock + control/event (original layout) or lock-free shards as jobs.
  // Keep existing thread roles; this only records that the third reserved slot is the control/event
  // thread under the default original layout (handshake shards still run inline on the tick thread).
  b.dedicatedExtra = orig ? 1u : 0u;

  // Detached software threeway (layout 8 default) spends the executor as a render-side thread.
  // Without threeway the caller is main, so only extra band workers count.
  b.renderSpawned = threeway ? p.swSpawnedDetached : p.swSpawned;

  if (dedOn) {
    b.meshDedicated = p.meshDedicated;
    // 0 is allowed under LL_HS_LAYOUT=original: shards already run inline, so the shared
    // reserve can be a no-thread, caller-runs pool (see cLockFreePool n==0).
    b.pool = p.reservePool;
  } else {
    b.meshDedicated = 0;
    b.pool = std::clamp(p.reservePool + p.meshDedicated, 1u, kMaxPoolWorkers);
  }
  b.activeCap = b.pool;
  return b;
}

unsigned computeWorkers(const ThreadBudget& b, unsigned renderSpawned) {
  return b.pool + b.meshDedicated + renderSpawned;
}

static const char* sourceName(CoresSource s) {
  return s == CoresSource::Cli ? "--cores" : s == CoresSource::Env ? "LITHO_CORES" : "hardware";
}

std::string threadTally(const ThreadBudget& b) {
  char buf[400];
  std::snprintf(buf, sizeof buf,
                "threads: census %u cores (%s; hardware reports %u): permanent 3 reserved "
                "(main, timing-handshake+control, tick/sim); remaining %u; render-side budget %u "
                "(remaining+2) used %u = mesh %u + reserve pool %u + render bands %u spawned; "
                "system/I-O threads not budgeted",
                b.hw, sourceName(b.source), b.hwRaw, b.remaining, b.renderSideBudget,
                computeWorkers(b, b.renderSpawned), b.meshDedicated, b.pool, b.renderSpawned);
  return buf;
}

std::string oversubscriptionWarning(const ThreadBudget& b, bool renderExplicit, unsigned renderSpawned) {
  const bool coresExplicit = b.source != CoresSource::Hardware;
  if (!coresExplicit && !renderExplicit) return {};  // automatic settings never warn (t3353u)
  const unsigned rs = renderExplicit ? renderSpawned : b.renderSpawned;
  const unsigned compute = computeWorkers(b, rs);
  const unsigned hwCores = std::max(1u, b.hwRaw);
  const ThreadBudget a = computeThreadBudget(hwCores);  // what automatic settings plan here
  const unsigned autoCompute = computeWorkers(a, a.renderSpawned);
  if (compute <= std::max(a.renderSideBudget, autoCompute)) return {};
  char buf[400];
  std::snprintf(buf, sizeof buf,
                "threads: explicit %s%s%s plans %u render-side workers (mesh %u + shared pool %u + render bands %u) on %u hardware "
                "cores (automatic settings plan %u, render-side budget %u); oversubscribing, no pinning (ruling 6)",
                coresExplicit ? sourceName(b.source) : "", coresExplicit && renderExplicit ? " + " : "",
                renderExplicit ? "--render-threads" : "", compute, b.meshDedicated, b.pool, rs, hwCores, autoCompute,
                a.renderSideBudget);
  return buf;
}

std::string poolInvariantViolation(size_t livePooled, const ThreadBudget& b) {
  if (livePooled <= b.pool) return {};
  char buf[200];
  std::snprintf(buf, sizeof buf, "threads: invariant violated: %zu live pool worker records exceed the census pool of %u",
                livePooled, b.pool);
  return buf;
}

uint64_t threadRecordHash(const ThreadRecord& r) {
  Fnv f;
  f.u64(r.id);
  f.byte(uint8_t(r.cls)); f.byte(uint8_t(r.role)); f.byte(uint8_t(r.state)); f.byte(uint8_t(r.reason));
  f.u64(r.roleIndex); f.u64(r.spawnTick); f.u64(r.returnTick); f.u64(r.ticket);
  return f.h;
}

cThreadCensus& cThreadCensus::instance() {
  // Never destroyed, so owners torn down during static destruction (band pool, stores,
  // the shared job pool) can still retire their records. JD t3349u item 5: placement-new
  // into static storage, not a heap leak, so leak sanitizers have nothing to report.
  static cThreadCensus* c = [] {
    g_hwReads.fetch_add(1, std::memory_order_relaxed);
    const unsigned hw = std::thread::hardware_concurrency();
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* env = std::getenv("LITHO_CORES");
    const CoresResolution r = resolveCores(g_cliCores.load(), env, hw);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (env && *env && r.source == CoresSource::Hardware)  // JD t3349u item 6
      logWarn("threads: invalid LITHO_CORES='%s' (want an integer 1..4096); falling back to %u cores (hardware)",
              env, r.cores);
    alignas(cThreadCensus) static unsigned char storage[sizeof(cThreadCensus)];
    auto* cc = new (storage) cThreadCensus(r.cores, r.source);
    cc->budget_.hwRaw = hw;  // telemetry: what the hardware reported
    g_booted.store(true);
    logInfo("%s", threadTally(cc->budget_).c_str());  // t3353u: the tally is a summary, not a warning
    if (cc->budget_.capped) {
      const ThreadBudget& b = cc->budget_;
      logInfo("threads: permanent 3 (main, timing-handshake+control, tick/sim) reserved; remaining %u of %u cores (%s); "
              "render-side budget %u (remaining+2) used %u = mesh %u + reserve pool %u + render bands %u spawned",
              b.remaining, b.hw, sourceName(b.source), b.renderSideBudget, b.renderSideUsed(),
              b.meshDedicated, b.pool, b.renderSpawned);
      if (b.renderSideUsed() > b.renderSideBudget)
        logWarn("threads: render-side used %u exceeds budget %u on %u cores (%s); oversubscribing, no pinning",
                b.renderSideUsed(), b.renderSideBudget, b.hw, sourceName(b.source));
    }
    const std::string w = oversubscriptionWarning(cc->budget_);  // explicit --cores / LITHO_CORES only
    if (!w.empty()) logWarn("%s", w.c_str());
    return cc;
  }();
  return *c;
}

unsigned cThreadCensus::hwReads() { return g_hwReads.load(std::memory_order_relaxed); }

cThreadCensus::cThreadCensus(unsigned hwRaw, CoresSource source) : budget_(computeThreadBudget(hwRaw)) {
  budget_.source = source;
}

unsigned cThreadCensus::renderBands(unsigned req) const {
  const unsigned n = resolveRenderBands(req, budget_.hw, kCensusWasm);
  if (req) {  // t3353u: an explicit --render-threads may warn, once per process
    static std::atomic<bool> warned{false};
    const std::string w = oversubscriptionWarning(budget_, true, n > 0 ? n - 1 : 0);
    if (!w.empty() && !warned.exchange(true)) logWarn("%s", w.c_str());
  }
  return n;
}

void cThreadCensus::noteCommittedTick(uint64_t tick) { g_lastCommittedTick.store(tick, std::memory_order_relaxed); }
uint64_t cThreadCensus::lastCommittedTick() { return g_lastCommittedTick.load(std::memory_order_relaxed); }

bool cThreadCensus::setCliCores(unsigned cores) {
  if (g_booted.load()) return false;
  g_cliCores.store(cores);
  return true;
}

uint32_t cThreadCensus::declare(ThreadClass cls, ThreadRole role, uint16_t roleIndex, const char* name) {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  auto s = std::make_unique<Slot>();
  s->rec.id = uint32_t(slots_.size());
  s->rec.cls = cls;
  s->rec.role = role;
  s->rec.roleIndex = roleIndex;
  s->rec.state = cls == ThreadClass::Dedicated ? ThreadState::Dedicated : ThreadState::Pooled;
  s->rec.spawnTick = 0;  // all OS threads are created at boot (tick 0)
  s->word.store(pack(s->rec.state, 0), std::memory_order_relaxed);
  s->tel.name = name ? name : "";
  slots_.push_back(std::move(s));
  return slots_.back()->rec.id;
}

bool cThreadCensus::tryClaim(uint32_t id, uint64_t tick) {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  if (id >= slots_.size()) return false;
  Slot& s = *slots_[id];
  if (s.rec.cls != ThreadClass::Pooled) return false;
  uint64_t w = s.word.load(std::memory_order_acquire);
  for (;;) {
    const ThreadState st = stateOf(w);
    if (st != ThreadState::Pooled && st != ThreadState::Reclaimed) return false;
    if (s.word.compare_exchange_weak(w, pack(ThreadState::Claimed, ticketOf(w) + 1), std::memory_order_acq_rel)) break;
  }
  s.rec.state = ThreadState::Claimed;
  s.rec.ticket = ticketOf(w) + 1;
  s.rec.spawnTick = tick;  // lend tick
  s.rec.reason = ThreadReturnReason::None;
  return true;
}

bool cThreadCensus::reclaim(uint32_t id, uint64_t tick) {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  if (id >= slots_.size()) return false;
  Slot& s = *slots_[id];
  uint64_t w = s.word.load(std::memory_order_acquire);
  if (stateOf(w) != ThreadState::Claimed) return false;
  if (!s.word.compare_exchange_strong(w, pack(ThreadState::Reclaimed, ticketOf(w)), std::memory_order_acq_rel)) return false;
  s.rec.state = ThreadState::Reclaimed;
  s.rec.returnTick = tick;
  s.rec.reason = ThreadReturnReason::Reclaimed;
  return true;
}

void cThreadCensus::returnAll(uint64_t tick) {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  for (auto& s : slots_) {
    s->word.store(pack(ThreadState::Returned, ticketOf(s->word.load(std::memory_order_relaxed))), std::memory_order_release);
    s->rec.state = ThreadState::Returned;
    s->rec.returnTick = tick;
    s->rec.reason = ThreadReturnReason::EnginePowerDown;
  }
}

void cThreadCensus::retire(uint32_t id, uint64_t tick) {
  if (tick == kLastCommittedTick) tick = lastCommittedTick();  // JD t3349u item 4
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  if (id >= slots_.size()) return;
  Slot& s = *slots_[id];
  if (s.rec.state == ThreadState::Returned) return;
  s.word.store(pack(ThreadState::Returned, ticketOf(s.word.load(std::memory_order_relaxed))), std::memory_order_release);
  s.rec.state = ThreadState::Returned;
  s.rec.returnTick = tick;
  s.rec.reason = ThreadReturnReason::EnginePowerDown;
}

size_t cThreadCensus::live() const {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  size_t n = 0;
  for (auto& s : slots_) n += s->rec.state != ThreadState::Returned;
  return n;
}

size_t cThreadCensus::live(ThreadClass cls) const {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  size_t n = 0;
  for (auto& s : slots_) n += s->rec.cls == cls && s->rec.state != ThreadState::Returned;
  return n;
}

void cThreadRecordSet::add(ThreadClass cls, ThreadRole role, uint16_t roleIndex, const char* name) {
  ids_.push_back(cThreadCensus::instance().declare(cls, role, roleIndex, name));
  retired_.push_back(0);
}

void cThreadRecordSet::retire(size_t i, uint64_t tick) {
  if (i >= ids_.size() || retired_[i]) return;
  cThreadCensus::instance().retire(ids_[i], tick);
  retired_[i] = 1;
}

void cThreadRecordSet::retireAll(uint64_t tick) {
  for (size_t i = 0; i < ids_.size(); ++i) retire(i, tick);
  ids_.clear();
  retired_.clear();
}

size_t cThreadCensus::size() const { litholattice::cGateGuard<litholattice::cAtomicGate> l(m_); return slots_.size(); }

ThreadRecord cThreadCensus::record(uint32_t id) const {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  return id < slots_.size() ? slots_[id]->rec : ThreadRecord{};
}

ThreadTelemetry cThreadCensus::telemetry(uint32_t id) const {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  return id < slots_.size() ? slots_[id]->tel : ThreadTelemetry{};
}

void cThreadCensus::setTelemetry(uint32_t id, const ThreadTelemetry& t) {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  if (id < slots_.size()) slots_[id]->tel = t;
}

uint64_t cThreadCensus::hash() const {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  Fnv f;
  for (auto& s : slots_) f.u64(threadRecordHash(s->rec));
  return f.h;
}

size_t cThreadCensus::count(ThreadState st) const {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(m_);
  size_t n = 0;
  for (auto& s : slots_) n += s->rec.state == st;
  return n;
}

size_t osThreadCount() {
#ifdef _WIN32
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  size_t n = 0;
  const DWORD pid = GetCurrentProcessId();
  if (Thread32First(snap, &te)) do { n += te.th32OwnerProcessID == pid; } while (Thread32Next(snap, &te));
  CloseHandle(snap);
  return n;
#else
  return 0;
#endif
}

}  // namespace litholattice
