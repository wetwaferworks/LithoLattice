// PlayN (JD Next #1): unique_lock replacement for cAtomicGate / cBlockGate (no <mutex>).
// One movable owner of a BasicLockable gate; gates spin, never park.
#pragma once
#include <utility>

namespace litholattice {

struct sDeferLock { explicit sDeferLock() = default; };
struct sTryToLock { explicit sTryToLock() = default; };
struct sAdoptLock { explicit sAdoptLock() = default; };
inline constexpr sDeferLock kDeferLock{};
inline constexpr sTryToLock kTryToLock{};
inline constexpr sAdoptLock kAdoptLock{};

template <typename Gate>
class cGateLock {
 public:
  cGateLock() noexcept = default;
  explicit cGateLock(Gate& g) : g_(&g) { g_->lock(); owns_ = true; }
  cGateLock(Gate& g, sDeferLock) noexcept : g_(&g) {}
  cGateLock(Gate& g, sTryToLock) : g_(&g) { owns_ = g_->try_lock(); }
  cGateLock(Gate& g, sAdoptLock) noexcept : g_(&g), owns_(true) {}
  ~cGateLock() { if (owns_) g_->unlock(); }
  cGateLock(const cGateLock&) = delete;
  cGateLock& operator=(const cGateLock&) = delete;
  cGateLock(cGateLock&& o) noexcept : g_(o.g_), owns_(o.owns_) { o.g_ = nullptr; o.owns_ = false; }
  cGateLock& operator=(cGateLock&& o) noexcept {
    if (this != &o) {
      if (owns_) g_->unlock();
      g_ = o.g_; owns_ = o.owns_;
      o.g_ = nullptr; o.owns_ = false;
    }
    return *this;
  }
  void lock() { g_->lock(); owns_ = true; }
  bool try_lock() { owns_ = g_->try_lock(); return owns_; }
  void unlock() { g_->unlock(); owns_ = false; }
  Gate* release() noexcept { Gate* g = g_; g_ = nullptr; owns_ = false; return g; }
  void swap(cGateLock& o) noexcept { std::swap(g_, o.g_); std::swap(owns_, o.owns_); }
  bool owns_lock() const noexcept { return owns_; }
  explicit operator bool() const noexcept { return owns_; }
  Gate* mutex() const noexcept { return g_; }

 private:
  Gate* g_ = nullptr;
  bool owns_ = false;
};

}  // namespace litholattice
