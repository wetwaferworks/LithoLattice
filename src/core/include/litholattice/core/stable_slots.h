// t46u item 5c (JD 2026-11-16): append-only table with stable addresses.
// One writer appends (and, rarely, replaces) entries; any number of readers
// read them with no lock and no wait. Entries are immutable heap objects
// reached through atomic pointers in fixed-size segments, so nothing ever
// moves: growth never copies the table (no quadratic boot cost) and a
// reader can never see a half-built or reallocated entry.
//
// Reclamation: none while the table lives. An entry replaced by replace()
// stays alive (owned_) until the table is destroyed, so a reader that loaded
// the old pointer can always finish its copy. Replacement is the debug
// hot-swap path only, so the retained memory is bounded by the number of
// hot-swapped definitions.
//
// Ordering: the writer stores the entry pointer (release) before the count
// (release); readers load the count (acquire) and then the pointer (acquire).
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace litholattice {

template <class T>
class cStableSlots {
 public:
  static constexpr std::size_t kSegBits = 10;
  static constexpr std::size_t kSegSize = std::size_t(1) << kSegBits;
  static constexpr std::size_t kMaxSegs = 1024;
  static constexpr std::size_t kCapacity = kSegSize * kMaxSegs;

  cStableSlots() = default;
  cStableSlots(const cStableSlots&) = delete;
  cStableSlots& operator=(const cStableSlots&) = delete;
  ~cStableSlots() {
    for (auto& s : segs_) delete s.load(std::memory_order_relaxed);
  }

  // ---- any thread ----
  std::size_t size() const { return count_.load(std::memory_order_acquire); }
  /// Entry i; the caller checks i < size() first.
  const T& at(std::size_t i) const {
    const Seg* s = segs_[i >> kSegBits].load(std::memory_order_acquire);
    return *s->p[i & (kSegSize - 1)].load(std::memory_order_acquire);
  }

  // ---- the one writer ----
  bool push(T v) {
    const std::size_t i = count_.load(std::memory_order_relaxed);
    if (i >= kCapacity) return false;
    Seg* s = segs_[i >> kSegBits].load(std::memory_order_relaxed);
    if (!s) {
      s = new Seg();
      segs_[i >> kSegBits].store(s, std::memory_order_release);
    }
    s->p[i & (kSegSize - 1)].store(keep(std::move(v)), std::memory_order_release);
    count_.store(i + 1, std::memory_order_release);
    return true;
  }
  /// Swap entry i (< size()) for a new immutable object; the old one stays alive.
  void replace(std::size_t i, T v) {
    Seg* s = segs_[i >> kSegBits].load(std::memory_order_relaxed);
    s->p[i & (kSegSize - 1)].store(keep(std::move(v)), std::memory_order_release);
  }

 private:
  struct Seg {
    std::atomic<const T*> p[kSegSize] = {};
  };
  const T* keep(T&& v) {
    owned_.push_back(std::make_unique<const T>(std::move(v)));
    return owned_.back().get();
  }
  std::atomic<Seg*> segs_[kMaxSegs] = {};
  std::atomic<std::size_t> count_{0};
  std::vector<std::unique_ptr<const T>> owned_;  // writer-only
};

}  // namespace litholattice