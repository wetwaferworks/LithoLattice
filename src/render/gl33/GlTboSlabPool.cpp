#include "GlTboSlabPool.h"

#include <algorithm>
#include "litholattice/core/log.h"

#ifndef GL_RGBA32UI
#define GL_RGBA32UI 0x8D70
#endif

namespace litholattice::gl33 {

cGlTboSlabPool::~cGlTboSlabPool() {
  for (Slab& s : slabs_) {
    if (s.tex) glDeleteTextures(1, &s.tex);
    if (s.buf) glDeleteBuffers(1, &s.buf);
  }
}

bool cGlTboSlabPool::init(int slabs) {
  if (!slabs_.empty()) return true;  // a program rebuild re-runs the canon setup: keep the pool
  GLint maxTexels = 0;
  glGetIntegerv(0x8C2B /* GL_MAX_TEXTURE_BUFFER_SIZE */, &maxTexels);
  if (maxTexels > 0 && size_t(maxTexels) < kSlabTexels) {
    logError("gl33: canonical TBO slab (%zu texels) exceeds GL_MAX_TEXTURE_BUFFER_SIZE (%d)", kSlabTexels, maxTexels);
    return false;
  }
  const int n = std::clamp(slabs, 1, 4);
  for (int i = 0; i < n; ++i)
    if (!addSlab()) return false;
  logInfo("gl33: canonical TBO slab pool: %d x 64 MiB preallocated", n);
  return true;
}

bool cGlTboSlabPool::addSlab() {
  if (slabs_.size() >= kMaxSlabs) return false;
  Slab s;
  glGenBuffers(1, &s.buf);
  glBindBuffer(GL_TEXTURE_BUFFER, s.buf);
  glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(kSlabTexels * kTexelBytes), nullptr, GL_STATIC_DRAW);
  glGenTextures(1, &s.tex);
  glBindTexture(GL_TEXTURE_BUFFER, s.tex);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, s.buf);
  s.free.reset(kSlabTexels);
  slabs_.push_back(std::move(s));
  return true;
}

std::optional<cGlTboSlabPool::Range> cGlTboSlabPool::alloc(size_t texels) {
  if (!texels || texels > kSlabTexels) return std::nullopt;
  for (uint32_t i = 0; i < slabs_.size(); ++i)
    if (auto f = slabs_[i].free.alloc(texels)) return Range{i, *f};
  if (!addSlab()) return std::nullopt;
  logInfo("gl33: canonical TBO slab pool grew to %zu slabs (no copy)", slabs_.size());
  const uint32_t i = uint32_t(slabs_.size() - 1);
  if (auto f = slabs_[i].free.alloc(texels)) return Range{i, *f};
  return std::nullopt;
}

void cGlTboSlabPool::release(const Range& r, size_t texels) {
  if (r.slab < slabs_.size()) slabs_[r.slab].free.release(r.first, texels);
}

void cGlTboSlabPool::upload(const Range& r, const void* texels, size_t count) {
  glBindBuffer(GL_TEXTURE_BUFFER, slabs_[r.slab].buf);
  glBufferSubData(GL_TEXTURE_BUFFER, GLintptr(r.first * kTexelBytes), GLsizeiptr(count * kTexelBytes), texels);
}

void cGlTboSlabPool::reset() {
  for (Slab& s : slabs_) s.free.reset(kSlabTexels);
}

}  // namespace litholattice::gl33
