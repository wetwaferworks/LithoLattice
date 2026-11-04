// cLfKeyTable (JD 2026-11-01): fixed-size lock-free open-addressed table of 64-bit keys to
// 64-bit atomic values. Insert-only keys (CAS on an empty slot), values updated with atomic
// fetch_or / fetch_add / exchange. Atomics only: no mutex. A probe that finds no slot within
// kMaxProbe returns false, and the caller takes its own lossless spill path.
// Used by LL_LF_COWTABLE (stale sub masks) and LL_LF_SAVE/LOAD (per-region pending saves).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace litholattice {

class cLfKeyTable {
 public:
  static constexpr int kMaxProbe = 64;
  explicit cLfKeyTable(size_t capacity = size_t(1) << 14) : mask_(roundPow2(capacity) - 1), slots_(new Slot[mask_ + 1]) {}
  cLfKeyTable(const cLfKeyTable&) = delete;
  cLfKeyTable& operator=(const cLfKeyTable&) = delete;

  /// value |= bits. False when the table had no slot for the key (spill).
  bool fetchOr(uint64_t key, uint64_t bits) {
    Slot* s = slot(key, true);
    if (!s) return false;
    s->v.fetch_or(bits, std::memory_order_acq_rel);
    return true;
  }
  /// value += delta (two's complement). False when the table had no slot for the key.
  bool add(uint64_t key, int64_t delta) {
    Slot* s = slot(key, true);
    if (!s) return false;
    s->v.fetch_add(uint64_t(delta), std::memory_order_acq_rel);
    return true;
  }
  /// Current value (0 when the key was never inserted).
  uint64_t get(uint64_t key) const {
    const Slot* s = const_cast<cLfKeyTable*>(this)->slot(key, false);
    return s ? s->v.load(std::memory_order_acquire) : 0;
  }
  /// For every slot with a nonzero value: exchange it to 0 and call f(key, oldValue).
  template <class F>
  void takeAll(F&& f) {
    for (size_t i = 0; i <= mask_; ++i) {
      const uint64_t k = slots_[i].k.load(std::memory_order_acquire);
      if (!k || !slots_[i].v.load(std::memory_order_relaxed)) continue;
      const uint64_t v = slots_[i].v.exchange(0, std::memory_order_acq_rel);
      if (v) f(k - 1, v);
    }
  }

 private:
  struct Slot {
    std::atomic<uint64_t> k{0};  // stored key + 1; 0 = empty
    std::atomic<uint64_t> v{0};
  };
  static size_t roundPow2(size_t n) { size_t c = 2; while (c < n) c <<= 1; return c; }
  static uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
  }
  Slot* slot(uint64_t key, bool insert) {
    if (key == ~uint64_t(0)) return nullptr;  // reserved (stored as key + 1)
    const uint64_t want = key + 1;
    size_t i = size_t(mix(key)) & mask_;
    for (int p = 0; p < kMaxProbe; ++p, i = (i + 1) & mask_) {
      uint64_t k = slots_[i].k.load(std::memory_order_acquire);
      if (k == want) return &slots_[i];
      if (k == 0) {
        if (!insert) return nullptr;
        if (slots_[i].k.compare_exchange_strong(k, want, std::memory_order_acq_rel)) return &slots_[i];
        if (k == want) return &slots_[i];
      }
    }
    return nullptr;
  }
  const size_t mask_;
  std::unique_ptr<Slot[]> slots_;
};

}  // namespace litholattice
