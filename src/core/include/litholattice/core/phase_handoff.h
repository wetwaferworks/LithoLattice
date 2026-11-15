#pragma once
// MUTEX-FREE (scratch, 2026-11-11, JD t5192u): ownership handoff queue drained at a phase edge.
//
// Any number of producer threads push(); exactly ONE consumer thread (the phase owner) calls
// drain()/clear() at its phase boundary. push is a lock-free CAS onto an intrusive stack; drain takes
// the WHOLE list with one atomic exchange (so there is no ABA: nodes are never popped one by one), then
// reverses it so items come out in push order (FIFO per producer, and global push-linearization order).
// Nothing is ever held across a wait; a producer never blocks the consumer and vice versa.
#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace litholattice {

template <class T>
class cPhaseHandoff {
 public:
  cPhaseHandoff() = default;
  cPhaseHandoff(const cPhaseHandoff&) = delete;
  cPhaseHandoff& operator=(const cPhaseHandoff&) = delete;
  ~cPhaseHandoff() { freeList(head_.exchange(nullptr, std::memory_order_acquire)); }

  /// Any thread. Hands ownership of `v` to the phase owner.
  void push(T v) {
    count_.fetch_add(1, std::memory_order_relaxed);  // before the publish, so the hint never goes below 0
    Node* n = new Node{std::move(v), head_.load(std::memory_order_relaxed)};
    while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {
    }
  }
  /// Phase owner only. Appends every handed-off item, in push order, to `out`.
  template <class Fn>
  void drain(Fn&& fn) {
    Node* n = head_.exchange(nullptr, std::memory_order_acquire);
    Node* rev = nullptr;
    while (n) { Node* nx = n->next; n->next = rev; rev = n; n = nx; }
    size_t taken = 0;
    while (rev) { Node* nx = rev->next; fn(std::move(rev->value)); delete rev; rev = nx; ++taken; }
    count_.fetch_sub((long long)taken, std::memory_order_relaxed);
  }
  /// Phase owner only. Discards everything handed off so far.
  void clear() { count_.fetch_sub((long long)freeList(head_.exchange(nullptr, std::memory_order_acquire)), std::memory_order_relaxed); }
  bool emptyHint() const { return head_.load(std::memory_order_relaxed) == nullptr; }
  /// Diagnostic only: items handed off and not yet drained (exact when producers are quiescent).
  size_t sizeHint() const { const long long c = count_.load(std::memory_order_relaxed); return c > 0 ? size_t(c) : 0; }

 private:
  struct Node {
    T value;
    Node* next;
  };
  static size_t freeList(Node* n) {
    size_t k = 0;
    while (n) { Node* nx = n->next; delete n; n = nx; ++k; }
    return k;
  }
  std::atomic<Node*> head_{nullptr};
  std::atomic<long long> count_{0};
};

}  // namespace litholattice
