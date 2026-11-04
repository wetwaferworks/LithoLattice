// Private to the software renderer: fix (e), Archie Q5. Bounds the expanded PackedVertex
// decode of canonical chunks (LL_SW_CANON=1) with an LRU cap. The canonical source stays
// resident; an evicted chunk is re-decoded the next frame it is in view. Bookkeeping only:
// the renderer owns the decoded store, so the bytes drawn are unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace litholattice {

class cSwDecodeCache {
 public:
  cSwDecodeCache();  ///< cap from LL_SW_DECODE_CAP_MB (default 512; 0 = unbounded)
  void setCapBytes(size_t cap) { cap_ = cap; }
  size_t capBytes() const { return cap_; }
  size_t residentBytes() const { return bytes_; }
  size_t residentCount() const { return entries_.size(); }

  void onDecoded(int64_t key, size_t bytes, uint64_t frame);  ///< (re)decode stored
  void onErased(int64_t key);                                 ///< store entry gone
  void clear() { entries_.clear(); bytes_ = 0; }
  void touch(int64_t key, uint64_t frame);                    ///< drawn this frame
  bool contains(int64_t key) const { return entries_.count(key) != 0; }
  /// Least-recently-drawn keys to evict until under the cap; never a key touched at `frame`.
  void collectEvictions(uint64_t frame, std::vector<int64_t>& out) const;

  uint64_t evictions() const { return evictions_; }
  uint64_t redecodes() const { return redecodes_; }
  void countEviction() { ++evictions_; }
  void countRedecode() { ++redecodes_; }

 private:
  struct Entry { size_t bytes = 0; uint64_t lastFrame = 0; };
  std::unordered_map<int64_t, Entry> entries_;
  size_t bytes_ = 0;
  size_t cap_ = 0;
  uint64_t evictions_ = 0, redecodes_ = 0;
};

}  // namespace litholattice
