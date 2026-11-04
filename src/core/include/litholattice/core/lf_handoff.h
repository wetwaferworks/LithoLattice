// cLfHandoff (JD 2026-11-01, LL_LF_INGRESS): unbounded lock-free multi-producer /
// single-consumer handoff. Producers CAS-push nodes onto an atomic head (Treiber); the
// single consumer takes the whole list with one atomic exchange (no ABA: nothing is
// popped node by node), then restores push order. Atomics only, no mutex.
#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace litholattice {

template <class T>
class cLfHandoff {
 public:
  cLfHandoff() = default;
  cLfHandoff(const cLfHandoff&) = delete;
  cLfHandoff& operator=(const cLfHandoff&) = delete;
  ~cLfHandoff() { free(head_.exchange(nullptr, std::memory_order_acquire)); }

  void push(T v) {
    Node* n = new Node{std::move(v), head_.load(std::memory_order_relaxed)};
    while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {}
    count_.fetch_add(1, std::memory_order_release);
  }
  /// Single consumer. Appends everything pushed so far, oldest first. Returns the count.
  size_t drainInto(std::vector<T>& out) {
    Node* n = head_.exchange(nullptr, std::memory_order_acquire);
    Node* rev = nullptr;
    size_t k = 0;
    while (n) { Node* nx = n->next; n->next = rev; rev = n; n = nx; ++k; }
    while (rev) { Node* nx = rev->next; out.push_back(std::move(rev->val)); delete rev; rev = nx; }
    if (k) count_.fetch_sub(k, std::memory_order_release);
    return k;
  }
  size_t sizeApprox() const { return count_.load(std::memory_order_acquire); }

 private:
  struct Node { T val; Node* next; };
  static void free(Node* n) { while (n) { Node* nx = n->next; delete n; n = nx; } }
  std::atomic<Node*> head_{nullptr};
  std::atomic<size_t> count_{0};
};

}  // namespace litholattice
