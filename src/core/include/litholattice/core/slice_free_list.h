#pragma once
// Voxy #5 (2026-10-31; Proposed by JD, Systems architecture: Archie, Built by Cortana): lock-free size-class
// free list of (block, offset) slices for the Vulkan vertex arena (LL_VK_ARENA_LF=1). One Treiber stack per
// power-of-two size class over a fixed node pool; heads carry a 32-bit tag against ABA. Nodes are never
// returned to the heap, so a stale reader of `next` reads a valid atomic. No mutex, no thread pinning.
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace litholattice {

class cSliceFreeList {
 public:
  static constexpr uint32_t kClasses = 40;
  struct Entry {
    uint32_t block = 0;
    uint64_t offset = 0;
  };
  explicit cSliceFreeList(uint32_t capacity);
  /// Pushes a free slice of class `cls`. false when the node pool is exhausted (the caller drops the slice).
  bool push(uint32_t cls, uint32_t block, uint64_t offset);
  /// Pops a free slice of class `cls`, or nullopt when that class is empty.
  std::optional<Entry> pop(uint32_t cls);
  /// Empties every class (not concurrent with push/pop).
  void reset();
  uint32_t capacity() const { return cap_; }
  /// Smallest c with (1 << c) >= units (units 0 or 1 -> 0).
  static uint32_t classOf(uint64_t units);

 private:
  struct Node {
    std::atomic<uint32_t> next{0};
    uint32_t block = 0;
    uint64_t offset = 0;
  };
  static constexpr uint32_t kNil = 0xFFFFFFFFu;
  void pushIdx(std::atomic<uint64_t>& head, uint32_t idx);
  uint32_t popIdx(std::atomic<uint64_t>& head);

  uint32_t cap_;
  std::unique_ptr<Node[]> nodes_;
  std::atomic<uint64_t> heads_[kClasses];
  std::atomic<uint64_t> spare_{kNil};  ///< unused nodes
};

}  // namespace litholattice
