// Thread Handshake P1: see thread_census.h. Record-only; nothing takes its counts yet.
#include "litholattice/core/thread_census.h"

#include <algorithm>
#include <cstdlib>
#include <thread>
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
  const unsigned want = std::min(req, kMaxRenderBands);
  unsigned b = 1;
  while (b * 2 <= want && b * 2 <= cores) b *= 2;
  return b;
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
  b.activeCap = b.pool;
  return b;
}

uint64_t threadRecordHash(const ThreadRecord& r) {
  Fnv f;
  f.u64(r.id);
  f.byte(uint8_t(r.cls)); f.byte(uint8_t(r.role)); f.byte(uint8_t(r.state)); f.byte(uint8_t(r.reason));
  f.u64(r.roleIndex); f.u64(r.spawnTick); f.u64(r.returnTick); f.u64(r.ticket);
  return f.h;
}

cThreadCensus& cThreadCensus::instance() {
  // P2: intentionally never destroyed, so owners torn down during static destruction
  // (band pool, stores) can still retire their records.
  static cThreadCensus* c = [] {
    g_hwReads.fetch_add(1, std::memory_order_relaxed);
    const unsigned hw = std::thread::hardware_concurrency();
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const CoresResolution r = resolveCores(g_cliCores.load(), std::getenv("LITHO_CORES"), hw);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    auto* cc = new cThreadCensus(r.cores, r.source);
    cc->budget_.hwRaw = hw;  // telemetry: what the hardware reported
    g_booted.store(true);
    return cc;
  }();
  return *c;
}

unsigned cThreadCensus::hwReads() { return g_hwReads.load(std::memory_order_relaxed); }

cThreadCensus::cThreadCensus(unsigned hwRaw, CoresSource source) : budget_(computeThreadBudget(hwRaw)) {
  budget_.source = source;
}

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
