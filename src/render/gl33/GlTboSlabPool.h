// Private to the gl33 backend (fix (c), 2026-10-30): paged pool of fixed 64 MiB RGBA32UI
// buffer-texture slabs for the M12 canonical pull streams (LL_GL_CANON). Replaces the
// doubling grow, whose glCopyBufferSubData of the whole old buffer ran on the frame thread.
// A range never spans slabs; a full pool adds a slab (no copy, existing ranges never move).
// 2 slabs are preallocated (LL_GL_CANON_SLABS=1..4 overrides). GL thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "TexelFreeList.h"
#include "gl_loader.h"

namespace litholattice::gl33 {

class cGlTboSlabPool {
 public:
  static constexpr size_t kTexelBytes = 16;                        // RGBA32UI
  static constexpr size_t kSlabTexels = (size_t(64) << 20) / kTexelBytes;  // 64 MiB
  static constexpr size_t kMaxSlabs = 16;

  struct Range { uint32_t slab = 0; size_t first = 0; };

  cGlTboSlabPool() = default;
  ~cGlTboSlabPool();
  cGlTboSlabPool(const cGlTboSlabPool&) = delete;
  cGlTboSlabPool& operator=(const cGlTboSlabPool&) = delete;

  /// Preallocate `slabs` slabs (clamped 1..4). False if the driver's TBO limit is too small.
  bool init(int slabs);
  std::optional<Range> alloc(size_t texels);
  void release(const Range& r, size_t texels);
  void upload(const Range& r, const void* texels, size_t count);
  /// Every range freed; slabs (and their VRAM) kept for reuse.
  void reset();
  GLuint texture(uint32_t slab) const { return slabs_[slab].tex; }
  size_t slabCount() const { return slabs_.size(); }

 private:
  struct Slab { GLuint buf = 0, tex = 0; cTexelFreeList free; };
  bool addSlab();
  std::vector<Slab> slabs_;
};

}  // namespace litholattice::gl33
