#include "litholattice/core/lock_timing.h"
#include "vk_vertex_arena.h"
#include <algorithm>
#include <iterator>
#include "litholattice/core/log.h"

namespace litholattice::vk {
namespace {
// 0.0.1.54 [A54-3a] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): 256 -> 64. Slices only need offset % sizeof(PackedVertex) (16) == 0
// for baseVertex = offset / 16; 64 keeps that and a cache line per slice start. Memory is
// HOST_COHERENT (no nonCoherentAtomSize rounding), vkCmdCopyBuffer has no offset alignment rule.
// 0.0.1.59 [P59-1] (Packed12; Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster
// and the Bitmasker; layout correction and build by Cortana): the alignment is per arena now: 64 for 16-byte
// vertices, 192 = lcm(12, 64) for the 12-byte Packed12 vertex (64 % 12 != 0 would make baseVertex fractional).
constexpr VkDeviceSize kAlignment16 = 64, kAlignment12 = 192;
static_assert(kAlignment16 % 16 == 0 && kAlignment12 % 12 == 0 && kAlignment12 % 16 == 0 && kAlignment12 % 64 == 0,
              "baseVertex = offset / stride");
VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }
}  // namespace

const char* cVertexArena::kindName(Kind k) {
  switch (k) {
    case Kind::Host: return "host";
    case Kind::Device: return "device";
    case Kind::ReBar: return "rebar";
  }
  return "host";
}

bool cVertexArena::parseKind(const std::string& s, Kind& out) {
  for (Kind k : {Kind::Host, Kind::Device, Kind::ReBar})
    if (s == kindName(k)) { out = k; return true; }
  return false;
}

cVertexArena::cVertexArena(const cVulkanDevice& dev, Kind kind, VkDeviceSize blockSize, VkDeviceSize alignment)
    : dev_(dev), kind_(kind), blockSize_(blockSize) {
  // 0.0.1.59 [P59-1]: only the two supported granularities; anything else falls back to the 16-byte one.
  alignment_ = (alignment == kAlignment12) ? kAlignment12 : kAlignment16;
  if (alignment != alignment_) logError("vulkan: vertex arena alignment %llu unsupported; using %llu", (unsigned long long)alignment, (unsigned long long)alignment_);
  blockSize_ = blockSize_ / alignment_ * alignment_;  // whole slices to the end of a block
  blocks_.resize(kMaxBlocks);  // [C53-2] fixed table: never reallocated while workers read it
  // 0.0.1.51 (Cortana, JD radius 64): keep vertex blocks within 70% of the biggest device-local
  // heap (room for textures, swapchain, bloom targets); past that, blocks go to host-visible
  // system memory the GPU reads over PCIe, so far rings are still drawn instead of dropped.
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(dev.physical(), &mp);
  VkDeviceSize heap = 0;
  for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
    if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max(heap, mp.memoryHeaps[i].size);
  deviceBudget_ = heap / 10 * 7;
  logInfo("vulkan: vertex arena device budget %llu MB (device-local heap %llu MB); overflow spills to host memory",
          (unsigned long long)(deviceBudget_ >> 20), (unsigned long long)(heap >> 20));
}

bool cVertexArena::addBlock(VkDeviceSize size) {
  constexpr auto kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (count_.load() >= kMaxBlocks) return false;  // [C53-2]
  std::optional<Buffer> buf;
  // 0.0.1.52 [A52-4] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): ReBAR blocks count against the device budget too.
  if (kind_ == Kind::ReBar && !(deviceBudget_ && deviceBytes_ + size > deviceBudget_)) {
    buf = Buffer::create(dev_, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | kHost);
    if (!buf) {
      logInfo("vulkan: no (more) host-visible device-local memory for the vertex arena; new blocks use device + staging");
      kind_ = Kind::Device;
    }
  }
  const bool overBudget = kind_ != Kind::Host && deviceBudget_ && deviceBytes_ + size > deviceBudget_;
  if (!buf && !overBudget) buf = kind_ == Kind::Host ? Buffer::create(dev_, size, usage, kHost)
                                                     : Buffer::create(dev_, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  bool host = kind_ == Kind::Host;
  if (!buf) {  // 0.0.1.51: over budget or device memory exhausted -> host-visible spill block
    buf = Buffer::create(dev_, size, usage, kHost);
    host = true;
    if (buf && !spillLogged_) {
      spillLogged_ = true;
      logInfo("vulkan: vertex arena past %llu MB device-local; new blocks spill to host memory",
              (unsigned long long)(deviceBytes_ >> 20));
    }
  }
  if (!buf) return false;
  (host ? hostBytes_ : deviceBytes_) += size;
  Block b;
  b.buffer = std::move(*buf);
  b.freeRanges.emplace(0, size);
  blocks_[count_.load()] = std::move(b);  // [C53-2] then publish the count
  count_.fetch_add(1);
  return true;
}

std::optional<cVertexArena::Slice> cVertexArena::allocate(VkDeviceSize bytes) {
  const VkDeviceSize size = alignUp(std::max<VkDeviceSize>(bytes, 1), alignment_);
  locktiming::TimedGuard lock(mu_, locktiming::kArena);  // [C53-2]; 0.0.1.60 timing (diagnostic)
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (uint32_t i = 0, n = count_.load(); i < n; ++i) {
      auto& ranges = blocks_[i].freeRanges;
      for (auto it = ranges.begin(); it != ranges.end(); ++it) {
        if (it->second < size) continue;
        const Slice s{i, it->first, size};
        const VkDeviceSize rest = it->second - size, restOffset = it->first + size;
        ranges.erase(it);
        if (rest) ranges.emplace(restOffset, rest);
        usedBytes_ += size;  // 0.0.1.59 [P59-1]
        return s;
      }
    }
    if (attempt == 0 && !addBlock(std::max(blockSize_, size))) return std::nullopt;
  }
  return std::nullopt;
}

void cVertexArena::free(const Slice& s) {
  locktiming::TimedGuard lock(mu_, locktiming::kArena);  // [C53-2]; 0.0.1.60 timing (diagnostic)
  auto& ranges = blocks_[s.block].freeRanges;
  usedBytes_ -= s.size;  // 0.0.1.59 [P59-1]
  auto it = ranges.emplace(s.offset, s.size).first;
  if (const auto next = std::next(it); next != ranges.end() && it->first + it->second == next->first) {
    it->second += next->second;
    ranges.erase(next);
  }
  if (it != ranges.begin()) {
    const auto prev = std::prev(it);
    if (prev->first + prev->second == it->first) {
      prev->second += it->second;
      ranges.erase(it);
    }
  }
}

void cVertexArena::clear() {
  locktiming::TimedGuard lock(mu_, locktiming::kArena);  // [C53-2]; 0.0.1.60 timing (diagnostic)
  for (uint32_t i = 0, n = count_.load(); i < n; ++i) blocks_[i] = Block{};
  count_ = 0;
  deviceBytes_ = 0; hostBytes_ = 0; usedBytes_ = 0;
}

}  // namespace litholattice::vk
