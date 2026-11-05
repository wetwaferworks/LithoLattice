#pragma once
// Thread Handshake P6 - swarm and locality (engine side).
//
//  * cSlab<T>: preallocated free list of reusable buffers. acquire() hands out a
//    shared_ptr whose deleter returns the buffer to the slab (or frees it if the
//    slab is gone or full), so a buffer may safely outlive its slab. Sized in
//    chunk-column rings: radius 4 = 81 columns, radius 8 = 289 columns.
//  * cSlackHarvester: turns spare frame time into a bounded number of extra
//    background slices (pure arithmetic + an EMA of the measured slice cost).
//
// Neither touches simulation state; both are scheduling aids only.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "litholattice/core/atomic_gate.h"

namespace litholattice {

constexpr size_t slabColumnsForRadius(int r) { return size_t(2 * r + 1) * size_t(2 * r + 1); }
constexpr size_t kSlabRadius4Columns = slabColumnsForRadius(4);  // 81
constexpr size_t kSlabRadius8Columns = slabColumnsForRadius(8);  // 289
static_assert(kSlabRadius4Columns == 81 && kSlabRadius8Columns == 289, "slab tiers");

template <class T>
class cSlab {
  struct State {
    litholattice::cAtomicGate m;
    std::vector<std::unique_ptr<T>> free;
    size_t cap = 0;
    std::atomic<uint64_t> hits{0}, misses{0}, returned{0}, dropped{0};
  };

 public:
  /// Preallocates `prealloc` buffers; retains at most `cap` returned buffers.
  explicit cSlab(size_t prealloc = 0, size_t cap = 0) : s_(std::make_shared<State>()) {
    s_->cap = std::max(prealloc, cap);
    reserve(prealloc);
  }
  /// Grow the preallocated pool to at least `n` idle buffers (raises the cap if needed).
  void reserve(size_t n) {
    std::vector<std::unique_ptr<T>> fresh;
    {
      litholattice::cGateGuard<litholattice::cAtomicGate> l(s_->m);
      s_->cap = std::max(s_->cap, n);
      if (s_->free.size() >= n) return;
      fresh.reserve(n - s_->free.size());
      for (size_t i = s_->free.size(); i < n; ++i) fresh.push_back(std::make_unique<T>());
      s_->free.reserve(s_->cap);
      for (auto& p : fresh) s_->free.push_back(std::move(p));
    }
  }
  std::shared_ptr<T> acquire() {
    std::unique_ptr<T> p;
    {
      litholattice::cGateGuard<litholattice::cAtomicGate> l(s_->m);
      if (!s_->free.empty()) { p = std::move(s_->free.back()); s_->free.pop_back(); }
    }
    if (p) s_->hits.fetch_add(1, std::memory_order_relaxed);
    else { p = std::make_unique<T>(); s_->misses.fetch_add(1, std::memory_order_relaxed); }
    std::weak_ptr<State> w = s_;
    return std::shared_ptr<T>(p.release(), [w](T* raw) {
      std::unique_ptr<T> owned(raw);
      if (auto s = w.lock()) {
        litholattice::cGateGuard<litholattice::cAtomicGate> l(s->m);
        if (s->free.size() < s->cap) {
          s->free.push_back(std::move(owned));
          s->returned.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        s->dropped.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  size_t idle() const { litholattice::cGateGuard<litholattice::cAtomicGate> l(s_->m); return s_->free.size(); }
  size_t cap() const { litholattice::cGateGuard<litholattice::cAtomicGate> l(s_->m); return s_->cap; }
  uint64_t hits() const { return s_->hits.load(std::memory_order_relaxed); }
  uint64_t misses() const { return s_->misses.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return s_->dropped.load(std::memory_order_relaxed); }

 private:
  std::shared_ptr<State> s_;
};

/// Frame-slack harvester: spare = budget - used; extra slices = floor(spare / cost),
/// clamped to [0, maxSlices]. The slice cost is an EMA of measured slices
/// (seeded by `initialCostMs`), so an expensive slice shrinks the next grant.
class cSlackHarvester {
 public:
  explicit cSlackHarvester(double initialCostMs = 0.25, int maxSlices = 8, double reserveMs = 2.0)
      : costMs_(initialCostMs), max_(maxSlices), reserveMs_(reserveMs) {}
  static int slicesFor(double budgetMs, double usedMs, double reserveMs, double costMs, int maxSlices) {
    const double spare = budgetMs - usedMs - reserveMs;
    if (!(spare > 0) || !(costMs > 0) || maxSlices <= 0) return 0;
    return int(std::min<double>(maxSlices, spare / costMs));
  }
  int grant(double budgetMs, double usedMs) const { return slicesFor(budgetMs, usedMs, reserveMs_, costMs_, max_); }
  void noteSlice(double ms) {
    if (ms >= 0) costMs_ = costMs_ * 0.875 + std::max(ms, 0.01) * 0.125;
  }
  double costMs() const { return costMs_; }

 private:
  double costMs_;
  int max_;
  double reserveMs_;
};

}  // namespace litholattice
