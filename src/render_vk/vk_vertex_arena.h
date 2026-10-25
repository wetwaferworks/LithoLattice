// cVertexArena: chunk vertex data sub-allocated from a few large buffers, so the number of Vulkan
// allocations stays tiny (drivers cap it, e.g. 4096) however many sub-chunks are loaded.
// First-fit free lists with coalescing. Private to render_vk.
// Kinds: Host (host-visible, written in place, the GPU reads over PCIe), Device (DEVICE_LOCAL,
// filled by vkCmdCopyBuffer from the renderer's staging ring), ReBar (DEVICE_LOCAL and
// host-visible, written in place; a block that cannot get such memory falls back to Device).
#pragma once
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "vk_resources.h"

namespace litholattice::vk {

class cVertexArena {
 public:
  /// A range of one block; `size` is the rounded-up allocation size.
  struct Slice {
    uint32_t block = 0;
    VkDeviceSize offset = 0, size = 0;
  };

  enum class Kind { Host, Device, ReBar };
  static const char* kindName(Kind k);
  /// "host" | "device" | "rebar"; false if unknown.
  static bool parseKind(const std::string& s, Kind& out);

  /// `alignment` (0.0.1.59 [P59-1]): slice offset/size granularity; must be a whole number of vertices
  /// (64 for the 16-byte PackedVertex, 192 = lcm(12, 64) for Packed12) so baseVertex = offset / stride.
  cVertexArena(const cVulkanDevice& dev, Kind kind, VkDeviceSize blockSize = VkDeviceSize(32) << 20, VkDeviceSize alignment = 64);
  // 0.0.1.53 [C53-2] (Proposed by JD, Built by Cortana): thread-safe. allocate/free/clear lock a mutex;
  // blocks live in a fixed table (never reallocated), so buffer()/data() of a slice the caller
  // owns need no lock. Mesh worker threads allocate + write ReBAR slices directly.
  Kind kind() const { return kind_.load(std::memory_order_relaxed); }

  /// Reserves `bytes` (a new block is added when none has room). nullopt if out of memory.
  /// 2026-10-25 fix pass (review P3): `maxBlock` limits the search and growth to blocks [0, maxBlock), so the
  /// pulled path (binding 6 holds kMaxPullBlocks blocks) never lands past it and has to free and redo.
  std::optional<Slice> allocate(VkDeviceSize bytes, uint32_t maxBlock = UINT32_MAX);
  /// Returns a slice; the caller guarantees the GPU no longer reads it.
  void free(const Slice& s);
  /// Drops every block (the caller guarantees the GPU is idle).
  void clear();

  VkBuffer buffer(const Slice& s) const { return blocks_[s.block].buffer.handle(); }
  /// Where to write the slice in place, or nullptr for a device-only block (stage + copy).
  uint8_t* data(const Slice& s) const {
    uint8_t* m = blocks_[s.block].buffer.mapped();
    return m ? m + s.offset : nullptr;
  }
  /// Mega-Pipeline Phase 2: the buffer of block i (i < blockCount()).
  VkBuffer blockBuffer(uint32_t i) const { return blocks_[i].buffer.handle(); }
  /// Acquire (review R5): pairs with the release in addBlock, so a reader never sees a count without its block.
  size_t blockCount() const { return count_.load(std::memory_order_acquire); }
  /// Nominal block size (review R3/R6). A single slice larger than this gets a block of its own size.
  VkDeviceSize blockSize() const { return blockSize_; }
  /// 0.0.1.51: bytes in device-local blocks / in host-visible spill blocks.
  VkDeviceSize deviceBytes() const { return deviceBytes_.load(std::memory_order_relaxed); }
  VkDeviceSize hostBytes() const { return hostBytes_.load(std::memory_order_relaxed); }
  /// 0.0.1.59 [P59-1]: bytes in live slices (mesh memory incl. alignment padding), and the alignment.
  VkDeviceSize usedBytes() const { return usedBytes_.load(std::memory_order_relaxed); }
  VkDeviceSize alignment() const { return alignment_; }

 private:
  struct Block {
    Buffer buffer;
    std::map<VkDeviceSize, VkDeviceSize> freeRanges;  ///< offset -> size, non-adjacent
  };
  bool addBlock(VkDeviceSize size);

  const cVulkanDevice& dev_;
  std::atomic<Kind> kind_;
  VkDeviceSize blockSize_;
  std::atomic<VkDeviceSize> deviceBytes_{0}, hostBytes_{0};
  std::atomic<VkDeviceSize> usedBytes_{0};  ///< 0.0.1.59 [P59-1]
  VkDeviceSize alignment_ = 64;  ///< 0.0.1.59 [P59-1]
  static constexpr uint32_t kMaxBlocks = 1024;  ///< [C53-2] fixed table (32 GB of 32 MB blocks)
  std::atomic<uint32_t> count_{0};
  std::mutex mu_;
  VkDeviceSize deviceBudget_ = 0;  ///< 0.0.1.51: device-local cap (70% of the largest device-local heap)
  bool spillLogged_ = false;
  std::vector<Block> blocks_;
};

}  // namespace litholattice::vk
