#pragma once
// Render pipeline M9 (Archie ruling 6): open-addressing hash table for the CoW ColumnTable.
// Linear probing over a power-of-two slot array (load <= 1/2), splitmix64 hash, backward-
// shift deletion (no tombstones). Keys are full 64-bit ChunkKeys (all values valid: an
// occupancy byte marks used slots). Not thread-safe (the ColumnTable mutex guards it).
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace litholattice {

template <class V>
class OaTable {
 public:
  V* find(uint64_t k) {
    if (used_.empty()) return nullptr;
    for (size_t i = slot(k);; i = (i + 1) & mask()) {
      if (!used_[i]) return nullptr;
      if (keys_[i] == k) return &vals_[i];
    }
  }
  const V* find(uint64_t k) const { return const_cast<OaTable*>(this)->find(k); }
  V& operator[](uint64_t k) {
    if (V* v = find(k)) return *v;
    if ((size_ + 1) * 2 > used_.size()) grow();
    size_t i = slot(k);
    while (used_[i]) i = (i + 1) & mask();
    used_[i] = 1; keys_[i] = k; vals_[i] = V{};
    ++size_;
    return vals_[i];
  }
  bool erase(uint64_t k) {
    if (used_.empty()) return false;
    size_t i = slot(k);
    for (;; i = (i + 1) & mask()) {
      if (!used_[i]) return false;
      if (keys_[i] == k) break;
    }
    // Backward-shift: pull later members of the probe run into the hole.
    size_t j = i;
    for (;;) {
      j = (j + 1) & mask();
      if (!used_[j]) break;
      const size_t h = slot(keys_[j]);
      // keys_[j] may move to i iff its home h is not cyclically in (i, j].
      const bool inRange = i <= j ? (h > i && h <= j) : (h > i || h <= j);
      if (inRange) continue;
      keys_[i] = keys_[j]; vals_[i] = std::move(vals_[j]);
      i = j;
    }
    used_[i] = 0; vals_[i] = V{};
    --size_;
    return true;
  }
  size_t size() const { return size_; }
  void clear() { used_.clear(); keys_.clear(); vals_.clear(); size_ = 0; }
  template <class F> void forEach(F&& f) {
    for (size_t i = 0; i < used_.size(); ++i) if (used_[i]) f(keys_[i], vals_[i]);
  }
  template <class F> void forEach(F&& f) const {
    for (size_t i = 0; i < used_.size(); ++i) if (used_[i]) f(keys_[i], vals_[i]);
  }
  /// Longest probe run (diagnostics / tests).
  size_t maxProbe() const {
    size_t m = 0;
    for (size_t i = 0; i < used_.size(); ++i)
      if (used_[i]) m = std::max(m, size_t((i - slot(keys_[i])) & mask()) + 1);
    return m;
  }

 private:
  static uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
  }
  size_t mask() const { return used_.size() - 1; }
  size_t slot(uint64_t k) const { return size_t(mix(k)) & mask(); }
  void grow() {
    std::vector<uint8_t> u = std::move(used_);
    std::vector<uint64_t> k = std::move(keys_);
    std::vector<V> v = std::move(vals_);
    const size_t n = u.empty() ? 64 : u.size() * 2;
    used_.assign(n, 0); keys_.assign(n, 0); vals_.clear(); vals_.resize(n);
    size_ = 0;
    for (size_t i = 0; i < u.size(); ++i)
      if (u[i]) (*this)[k[i]] = std::move(v[i]);
  }
  std::vector<uint8_t> used_;
  std::vector<uint64_t> keys_;
  std::vector<V> vals_;
  size_t size_ = 0;
};

}  // namespace litholattice