// Thread Handshake P1: see thread_census.h. Record-only; nothing takes its counts yet.
#include "litholattice/core/thread_census.h"

#include <algorithm>
#include <thread>

namespace litholattice {

namespace {
std::atomic<unsigned> g_hwReads{0};

constexpr uint64_t pack(ThreadState s, uint32_t ticket) { return (uint64_t(ticket) << 8) | uint64_t(s); }
constexpr ThreadState stateOf(uint64_t w) { return ThreadState(uint8_t(w & 0xff)); }
constexpr uint32_t ticketOf(uint64_t w) { return uint32_t(w >> 8); }

struct Fnv {
  uint64_t h = 1469598103934665603ULL;
  void byte(uint8_t b) { h ^= b; h *= 1099511628211ULL; }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) byte(uint8_t(v >> (8 * i))); }
};
}  // namespace

ThreadBudget computeThreadBudget(unsigned hwRaw) {
  ThreadBudget b;
  b.hwRaw = hwRaw;
  b.hw = std::max(1u, hwRaw);
  b.smallMachine = b.hw <= 4;
  // Class A: today's rules, unchanged (timing_handshake.cpp ctor; swAutoRenderThreads).
  b.simShards = std::min(4u, b.hw > 1 ? b.hw - 1 : 1u);
  b.renderBands = std::clamp(b.hw > 1 ? b.hw - 1 : 1u, 2u, 16u);
  b.renderSpawned = b.renderBands - 1;
  // Class B: on big machines leave one core for main; on <= 4 cores the dedicated
  // main + sim clock claim their cores first and the pool takes what is left (floor 1).
  b.pool = b.smallMachine ? (b.hw > b.dedicatedCores() ? b.hw - b.dedicatedCores() : 1u) : b.hw - 1;
  // Adaptive-cap seed: half the pool on small machines (render must keep a core), else the pool.
  b.activeCap = b.smallMachine ? std::max(1u, b.pool / 2) : b.pool;
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
  static cThreadCensus c([] {
    g_hwReads.fetch_add(1, std::memory_order_relaxed);
    return std::thread::hardware_concurrency();
  }());
  return c;
}

unsigned cThreadCensus::hwReads() { return g_hwReads.load(std::memory_order_relaxed); }

cThreadCensus::cThreadCensus(unsigned hwRaw) : budget_(computeThreadBudget(hwRaw)) {}

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

}  // namespace litholattice
