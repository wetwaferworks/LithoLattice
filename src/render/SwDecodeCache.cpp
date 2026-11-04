#include "litholattice/render/SwDecodeCache.h"

#include <algorithm>
#include <cstdlib>

namespace litholattice {

cSwDecodeCache::cSwDecodeCache() {
  const char* e = std::getenv("LL_SW_DECODE_CAP_MB");
  const long mb = e ? std::atol(e) : 512;
  cap_ = mb > 0 ? size_t(mb) << 20 : 0;
}

void cSwDecodeCache::onDecoded(int64_t key, size_t bytes, uint64_t frame) {
  Entry& en = entries_[key];
  bytes_ = bytes_ - en.bytes + bytes;
  en.bytes = bytes;
  en.lastFrame = frame;
}

void cSwDecodeCache::onErased(int64_t key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;
  bytes_ -= it->second.bytes;
  entries_.erase(it);
}

void cSwDecodeCache::touch(int64_t key, uint64_t frame) {
  auto it = entries_.find(key);
  if (it != entries_.end()) it->second.lastFrame = frame;
}

void cSwDecodeCache::collectEvictions(uint64_t frame, std::vector<int64_t>& out) const {
  out.clear();
  if (!cap_ || bytes_ <= cap_) return;
  std::vector<std::pair<uint64_t, int64_t>> lru;  // (lastFrame, key), oldest first
  lru.reserve(entries_.size());
  for (const auto& [k, en] : entries_)
    if (en.lastFrame != frame) lru.emplace_back(en.lastFrame, k);
  std::sort(lru.begin(), lru.end());
  size_t over = bytes_ - cap_;
  for (const auto& [f, k] : lru) {
    if (!over) break;
    const size_t b = entries_.at(k).bytes;
    out.push_back(k);
    over = b >= over ? 0 : over - b;
  }
}

}  // namespace litholattice
