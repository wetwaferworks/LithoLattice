// Gate 0: RAII charge against cMemoryLedger. Move-only; the destructor (or reset) gives the bytes
// back. Lets a resource class carry its ledger charge without a custom destructor.
#pragma once
#include <cstdint>
#include <utility>
#include "litholattice/core/memory_ledger.h"
namespace litholattice {
class cMemoryCharge {
 public:
  cMemoryCharge() = default;
  cMemoryCharge(cMemoryLedger::Category c, int64_t n) : cat_(c), n_(n) { if (n_) cMemoryLedger::add(cat_, n_); }
  cMemoryCharge(cMemoryCharge&& o) noexcept : cat_(o.cat_), n_(std::exchange(o.n_, 0)) {}
  cMemoryCharge& operator=(cMemoryCharge&& o) noexcept {
    if (this != &o) { reset(); cat_ = o.cat_; n_ = std::exchange(o.n_, 0); }
    return *this;
  }
  cMemoryCharge(const cMemoryCharge&) = delete;
  cMemoryCharge& operator=(const cMemoryCharge&) = delete;
  ~cMemoryCharge() { reset(); }
  void reset() { if (n_) { cMemoryLedger::sub(cat_, n_); n_ = 0; } }
  int64_t bytes() const { return n_; }

 private:
  cMemoryLedger::Category cat_ = cMemoryLedger::kCount;
  int64_t n_ = 0;
};
}  // namespace litholattice
