// t46u item 5 (JD 2026-11-16): one hot atomic per cache line.
// cPaddedAtomic<T> is a std::atomic<T> that owns a whole 64-byte line, so two
// threads hammering neighbouring counters (barrier words, per-shard done
// slots, reader counts) stop invalidating each other's line. Same interface
// as std::atomic for the calls the engine uses, so call sites do not change.
#pragma once
#include <atomic>
#include <cstddef>

namespace litholattice {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
struct alignas(kCacheLine) cPaddedAtomic {
  std::atomic<T> v;
  constexpr cPaddedAtomic() noexcept : v{} {}
  constexpr cPaddedAtomic(T x) noexcept : v{x} {}  // NOLINT: implicit like std::atomic
  cPaddedAtomic(const cPaddedAtomic&) = delete;
  cPaddedAtomic& operator=(const cPaddedAtomic&) = delete;
  T load(std::memory_order o = std::memory_order_seq_cst) const noexcept { return v.load(o); }
  void store(T x, std::memory_order o = std::memory_order_seq_cst) noexcept { v.store(x, o); }
  T exchange(T x, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.exchange(x, o); }
  bool compare_exchange_weak(T& e, T d, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.compare_exchange_weak(e, d, o); }
  bool compare_exchange_strong(T& e, T d, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.compare_exchange_strong(e, d, o); }
  bool compare_exchange_weak(T& e, T d, std::memory_order s, std::memory_order f) noexcept { return v.compare_exchange_weak(e, d, s, f); }
  bool compare_exchange_strong(T& e, T d, std::memory_order s, std::memory_order f) noexcept { return v.compare_exchange_strong(e, d, s, f); }
  T fetch_add(T x, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.fetch_add(x, o); }
  T fetch_sub(T x, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.fetch_sub(x, o); }
  T fetch_or(T x, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.fetch_or(x, o); }
  T fetch_and(T x, std::memory_order o = std::memory_order_seq_cst) noexcept { return v.fetch_and(x, o); }
  operator T() const noexcept { return v.load(); }  // NOLINT
  T operator=(T x) noexcept { v.store(x); return x; }
  T operator++() noexcept { return v.fetch_add(1) + 1; }
  T operator++(int) noexcept { return v.fetch_add(1); }
  T operator--() noexcept { return v.fetch_sub(1) - 1; }
  T operator--(int) noexcept { return v.fetch_sub(1); }
  T operator+=(T x) noexcept { return v.fetch_add(x) + x; }
  T operator-=(T x) noexcept { return v.fetch_sub(x) - x; }
  // wait/notify intentionally not forwarded: t46u removes atomic::wait parking.
};
static_assert(sizeof(cPaddedAtomic<unsigned long long>) == kCacheLine, "one line per hot atomic");

}  // namespace litholattice
