#include "litholattice/core/slice_free_list.h"

namespace litholattice {

cSliceFreeList::cSliceFreeList(uint32_t capacity) : cap_(capacity ? capacity : 1), nodes_(new Node[cap_ ? cap_ : 1]) {
  reset();
}

void cSliceFreeList::reset() {
  for (auto& h : heads_) h.store(kNil, std::memory_order_relaxed);
  for (uint32_t i = 0; i < cap_; ++i) nodes_[i].next.store(i + 1 < cap_ ? i + 1 : kNil, std::memory_order_relaxed);
  spare_.store(0, std::memory_order_release);
}

uint32_t cSliceFreeList::classOf(uint64_t units) {
  uint32_t c = 0;
  while (c < 63 && (uint64_t(1) << c) < units) ++c;
  return c;
}

void cSliceFreeList::pushIdx(std::atomic<uint64_t>& head, uint32_t idx) {
  uint64_t h = head.load(std::memory_order_relaxed);
  for (;;) {
    nodes_[idx].next.store(uint32_t(h), std::memory_order_relaxed);
    const uint64_t nh = (((h >> 32) + 1) << 32) | idx;
    if (head.compare_exchange_weak(h, nh, std::memory_order_release, std::memory_order_relaxed)) return;
  }
}

uint32_t cSliceFreeList::popIdx(std::atomic<uint64_t>& head) {
  uint64_t h = head.load(std::memory_order_acquire);
  for (;;) {
    const uint32_t idx = uint32_t(h);
    if (idx == kNil) return kNil;
    const uint32_t next = nodes_[idx].next.load(std::memory_order_relaxed);
    const uint64_t nh = (((h >> 32) + 1) << 32) | next;
    if (head.compare_exchange_weak(h, nh, std::memory_order_acquire, std::memory_order_acquire)) return idx;
  }
}

bool cSliceFreeList::push(uint32_t cls, uint32_t block, uint64_t offset) {
  if (cls >= kClasses) return false;
  const uint32_t idx = popIdx(spare_);
  if (idx == kNil) return false;
  nodes_[idx].block = block;
  nodes_[idx].offset = offset;
  pushIdx(heads_[cls], idx);
  return true;
}

std::optional<cSliceFreeList::Entry> cSliceFreeList::pop(uint32_t cls) {
  if (cls >= kClasses) return std::nullopt;
  const uint32_t idx = popIdx(heads_[cls]);
  if (idx == kNil) return std::nullopt;
  const Entry e{nodes_[idx].block, nodes_[idx].offset};
  pushIdx(spare_, idx);
  return e;
}

}  // namespace litholattice
