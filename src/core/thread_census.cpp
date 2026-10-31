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

unsigned autoRenderBands(unsigned cores, bool wasm) {
  const unsigned n = std::clamp(cores > 1 ? cores - 1 : 1u, 2u, 16u);
  return wasm ? std::min(n, 4u) : n;  // web: Archie's raster lane count, caller included
}

unsigned resolveRenderBands(unsigned req, unsigned cores, bool wasm) {
  cores = std::max(1u, cores);
  if (!req) return autoRenderBands(cores, wasm);  // auto: not limited by the hw clamp (2-core floor is 2)
  // JD t3349u item 7: power-of-two was only a heuristic (rows are pulled as 32-row strips
  // by any band count; the auto rule already yields 11 on 12 cores), so an explicit request
  // is honoured as given. The one hard limit is the 64-band storage cap.
  return std::min(req, kMaxRenderBands);
}

ThreadBudget computeThreadBudget(unsigned hwRaw, bool wasm) {
  ThreadBudget b;
  b.hwRaw = hwRaw;
  b.hw = std::max(1u, hwRaw);
  b.smallMachine = b.hw <= 4;
  // Class A: today's rules, unchanged (timing_handshake.cpp ctor; swAutoRenderThreads).
  b.simShards = std::min(4u, b.hw > 1 ? b.hw - 1 : 1u);
  b.renderBands = autoRenderBands(b.hw, wasm);
  b.renderSpawned = b.renderBands - 1;
  // Class B (Archie ruling 3/4): clamp(cores - 2, 1, kMaxPoolWorkers) on every size.
  b.pool = std::clamp(b.hw > 2 ? b.hw - 2 : 1u, 1u, kMaxPoolWorkers);
  // 2026-10-30 (JD t3557u) LL_THREAD_BUDGET=1 (implied by LL_HS_LOCKFREE=1 / LL_HS_LAYOUT=original):
  // total compute threads <= cores. Dedicated: main + sim clock, plus the lock-free shard threads
  // (LL_HS_LOCKFREE) or the control/event thread (original layout). The pool gets the rest.
  // Render bands are spawned lazily by the software renderer only, so Vulkan spends none.
  {
    const char* tb = std::getenv("LL_THREAD_BUDGET");
    const char* lf = std::getenv("LL_HS_LOCKFREE");
    const char* ly = std::getenv("LL_HS_LAYOUT");
    const bool orig = ly && std::strcmp(ly, "original") == 0;
    const bool lock = lf && lf[0] == '1';
    if ((tb && tb[0] == '1') || lock || orig) {
      const unsigned dedicated = 2u + (orig ? 1u : lock ? b.simShards : 0u);
      b.pool = std::clamp(b.hw > dedicated ? b.hw - dedicated : 1u, 1u, kMaxPoolWorkers);
      b.capped = true; b.dedicatedExtra = dedicated - 2u; b.layoutOriginal = orig;
    }
  }
  // Step 10 (JD 2026-10-31, default off): LL_DED_LAYOUT=6|8 dedicated layouts. 3 permanent
  // threads (main, tick, control+event: run with LL_HS_LAYOUT=original), a dedicated render
  // thread (LL_THREEWAY=1) and 2 dedicated mesh/vertex workers (the game's own lock-free
  // job pool, main.cpp). The shared pool keeps 2 workers for layout 8 and its 1-worker floor
  // for layout 6 (idle there: shards run inline on the tick thread).
  if (const char* d = std::getenv("LL_DED_LAYOUT")) {
    if (d[0] == '6' || d[0] == '8') { b.pool = d[0] == '8' ? 2u : 1u; b.capped = true; }
  }
  b.activeCap = b.pool;
  return b;
}

unsigned computeWorkers(const ThreadBudget& b, unsigned renderSpawned) { return b.pool + renderSpawned; }

static const char* sourceName(CoresSource s) {
  return s == CoresSource::Cli ? "--cores" : s == CoresSource::Env ? "LITHO_CORES" : "hardware";
}

std::string threadTally(const ThreadBudget& b) {
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "threads: census %u cores (%s; hardware reports %u): compute workers %u = shared pool %u + "
                "render bands %u spawned; dedicated main %u + sim clock %u; system/I-O threads not budgeted",
                b.hw, sourceName(b.source), b.hwRaw, computeWorkers(b, b.renderSpawned), b.pool, b.renderSpawned,
                b.mainThreads, b.simClock);
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
  if (compute <= std::max(hwCores, autoCompute)) return {};
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "threads: explicit %s%s%s plans %u compute workers (shared pool %u + render bands %u) on %u hardware "
                "cores (automatic settings plan %u); oversubscribing, no pinning (ruling 6)",
                coresExplicit ? sourceName(b.source) : "", coresExplicit && renderExplicit ? " + " : "",
                renderExplicit ? "--render-threads" : "", compute, b.pool, rs, hwCores, autoCompute);
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
    if (cc->budget_.capped) {  // 2026-10-30 (JD t3557u): budget never exceeds the core count (--cores > LITHO_CORES > hardware)
      const ThreadBudget& b = cc->budget_;
      const unsigned total = 2u + b.dedicatedExtra + b.pool;
      logInfo("threads: budget capped at %u cores (%s): main 1 + sim clock 1 + %s %u + on-demand pool %u = %u; render bands run on the pool (software renderer spawns %u lazily)",
              b.hw, sourceName(b.source), b.layoutOriginal ? "control/event" : "lock-free shards", b.dedicatedExtra, b.pool, total, b.renderSpawned);
      if (total > b.hw)
        logWarn("threads: budget %u exceeds %u cores (%s): main 1 + sim clock 1 + dedicated %u + pool %u (pool floor 1); oversubscribing, no pinning",
                total, b.hw, sourceName(b.source), b.dedicatedExtra, b.pool);
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
  std::lock_guard<std::mutex> l(m_);
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
  std::lock_guard<std::mutex> l(m_);
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
  std::lock_guard<std::mutex> l(m_);
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
  std::lock_guard<std::mutex> l(m_);
  for (auto& s : slots_) {
    s->word.store(pack(ThreadState::Returned, ticketOf(s->word.load(std::memory_order_relaxed))), std::memory_order_release);
    s->rec.state = ThreadState::Returned;
    s->rec.returnTick = tick;
    s->rec.reason = ThreadReturnReason::EnginePowerDown;
  }
}

void cThreadCensus::retire(uint32_t id, uint64_t tick) {
  if (tick == kLastCommittedTick) tick = lastCommittedTick();  // JD t3349u item 4
  std::lock_guard<std::mutex> l(m_);
  if (id >= slots_.size()) return;
  Slot& s = *slots_[id];
  if (s.rec.state == ThreadState::Returned) return;
  s.word.store(pack(ThreadState::Returned, ticketOf(s.word.load(std::memory_order_relaxed))), std::memory_order_release);
  s.rec.state = ThreadState::Returned;
  s.rec.returnTick = tick;
  s.rec.reason = ThreadReturnReason::EnginePowerDown;
}

size_t cThreadCensus::live() const {
  std::lock_guard<std::mutex> l(m_);
  size_t n = 0;
  for (auto& s : slots_) n += s->rec.state != ThreadState::Returned;
  return n;
}

size_t cThreadCensus::live(ThreadClass cls) const {
  std::lock_guard<std::mutex> l(m_);
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

size_t cThreadCensus::size() const { std::lock_guard<std::mutex> l(m_); return slots_.size(); }

ThreadRecord cThreadCensus::record(uint32_t id) const {
  std::lock_guard<std::mutex> l(m_);
  return id < slots_.size() ? slots_[id]->rec : ThreadRecord{};
}

ThreadTelemetry cThreadCensus::telemetry(uint32_t id) const {
  std::lock_guard<std::mutex> l(m_);
  return id < slots_.size() ? slots_[id]->tel : ThreadTelemetry{};
}

void cThreadCensus::setTelemetry(uint32_t id, const ThreadTelemetry& t) {
  std::lock_guard<std::mutex> l(m_);
  if (id < slots_.size()) slots_[id]->tel = t;
}

uint64_t cThreadCensus::hash() const {
  std::lock_guard<std::mutex> l(m_);
  Fnv f;
  for (auto& s : slots_) f.u64(threadRecordHash(s->rec));
  return f.h;
}

size_t cThreadCensus::count(ThreadState st) const {
  std::lock_guard<std::mutex> l(m_);
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
