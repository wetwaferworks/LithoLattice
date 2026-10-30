// Private to the gl33 backend: first-fit free list over a texel range (fix (c), split out
// of the M12 canonical TBO uploader). Adjacent ranges coalesce on release.
#pragma once
#include <cstddef>
#include <map>
#include <optional>

namespace litholattice::gl33 {

class cTexelFreeList {
 public:
  void reset(size_t capacity) { free_.clear(); if (capacity) free_.emplace(size_t(0), capacity); }
  std::optional<size_t> alloc(size_t n);
  void release(size_t first, size_t n);

 private:
  std::map<size_t, size_t> free_;  // first texel -> count
};

}  // namespace litholattice::gl33
