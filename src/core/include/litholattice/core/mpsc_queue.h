// Multi-producer / single-consumer injection queue (P0.5 review fix 2, 2026-10-29).
//
// Why: cJobSystem::submit() used to push from ANY thread onto the bottom of a worker's
// Chase-Lev deque, an owner-only end, and kept that safe with a per-deque mutex
// (external review by Cryosis (sirlinhart)). Now external submits go into the target
// worker's injection queue; only the owner pushes or pops its deque's bottom.
//
// Algorithm: Dmitry Vyukov's intrusive MPSC node queue.
//   push (any thread, wait-free): node->next = null; prev = head.exchange(node); prev->next = node
//   pop  (one consumer at a time): next = tail->next; if null -> empty; take next's value
// A push is visible to pop only after its link store; between the exchange and the link
// the queue briefly looks empty (the producer finishes in a few instructions). cJobSystem
// bumps its wake generation AFTER push() returns, so a sleeping worker cannot miss it.
//
// Single consumer: the consumer side is serialized by a try-claim flag (tryClaim/release),
// never a blocking lock. The owner worker normally drains its own queue; an idle worker
// may claim a busy worker's queue and drain it into its OWN deque, so injected work is
// never stranded behind a long job.
#pragma once

#include <atomic>
#include <utility>

namespace litholattice {

template <typename T>
class cMpscQueue {
 public:
  cMpscQueue() : head_(&stub_), tail_(&stub_) {}
  ~cMpscQueue() {  // no producers or consumer may be active (threads joined)
    T tmp;
    while (popUnclaimed(tmp)) {
    }
    if (tail_ != &stub_) delete tail_;  // the last consumed node is the current stub
  }
  cMpscQueue(const cMpscQueue&) = delete;
  cMpscQueue& operator=(const cMpscQueue&) = delete;

  /// Any thread. Never blocks; allocates one node.
  void push(T&& v) {
    Node* n = new Node(std::move(v));
    count_.fetch_add(1, std::memory_order_relaxed);
    Node* prev = head_.exchange(n, std::memory_order_acq_rel);
    prev->next.store(n, std::memory_order_release);
  }

  /// Become the (single) consumer. Returns false if another thread is consuming.
  bool tryClaim() { return !claimed_.exchange(true, std::memory_order_acquire); }
  void release() { claimed_.store(false, std::memory_order_release); }

  /// Consumer only (between a successful tryClaim and release).
  bool pop(T& out) { return popUnclaimed(out); }

  /// Racy hint: true if something may be queued. Never use it for correctness.
  /// Counter-based (never dereferences a node another thread may free).
  bool maybeNonEmpty() const { return count_.load(std::memory_order_relaxed) > 0; }

 private:
  struct Node {
    Node() = default;
    explicit Node(T&& v) : value(std::move(v)) {}
    std::atomic<Node*> next{nullptr};
    T value{};
  };
  bool popUnclaimed(T& out) {
    Node* tail = tail_;
    Node* next = tail->next.load(std::memory_order_acquire);
    if (!next) return false;
    out = std::move(next->value);
    count_.fetch_sub(1, std::memory_order_relaxed);
    tail_ = next;  // next becomes the new stub
    if (tail != &stub_) delete tail;
    return true;
  }
  Node stub_;
  std::atomic<Node*> head_;
  Node* tail_;  // consumer-owned
  std::atomic<bool> claimed_{false};
  std::atomic<long long> count_{0};
};

}  // namespace litholattice