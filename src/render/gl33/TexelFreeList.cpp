#include "TexelFreeList.h"

#include <iterator>

namespace litholattice::gl33 {

std::optional<size_t> cTexelFreeList::alloc(size_t n) {
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < n) continue;
    const size_t first = it->first, rest = it->second - n;
    free_.erase(it);
    if (rest) free_.emplace(first + n, rest);
    return first;
  }
  return std::nullopt;
}

void cTexelFreeList::release(size_t first, size_t n) {
  if (!n) return;
  auto it = free_.emplace(first, n).first;
  if (const auto next = std::next(it); next != free_.end() && it->first + it->second == next->first) {
    it->second += next->second;
    free_.erase(next);
  }
  if (it != free_.begin())
    if (const auto prev = std::prev(it); prev->first + prev->second == it->first) {
      prev->second += it->second;
      free_.erase(it);
    }
}

}  // namespace litholattice::gl33
