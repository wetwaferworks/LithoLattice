#pragma once
// Three-way split (Design B, Stage 2): the software renderer's upload ring.
// Proposed by JD; co-implemented by JD and Cortana (the split), Stage 2 by Elfie.
//
// A single-producer / single-consumer lock-free ring that stages chunk-mesh
// uploads between the game's upload phase (producer: the main thread — Game's
// uploadMeshes/retire paths and cBenchStreamer::update all run there) and the
// software renderer's mesh store (consumer: the band pool's detached executor,
// which drains the ring at the frame seal, immediately before renderFrame
// builds its draw order). JD's ruling 4: nobody waits in line on the CPU —
// the producer never blocks, takes no lock, and never touches the store on
// this path; the consumer is the store's only writer between seals.
//
// Argon's three traps (counsel pt 5, verified 2026-10-17) are binding spec:
//  1. No false sharing: the producer-published cursor (tail_) and the
//     consumer-published cursors (head_, consumedBytes_) sit on separate
//     alignas(64) cache lines; the announce counter gets its own line.
//  2. Coalescing is consumer-side at drain (in cSoftwareRenderer): a later
//     entry for a key supersedes an earlier undrained one; superseded meshes
//     are skipped whole (no face-bucket/emitter work runs for them — the
//     store entry points are only called for survivors). Tombstones are
//     never dropped by backpressure: they (and clears) are must-push, and
//     the game keeps a spill queue behind a full ring, so a removal is only
//     ever superseded by a NEWER entry for its own key — never lost.
//  3. No frees on the critical path: the consumer moves drained meshes into
//     a retire batch whose buffers are released after renderFrame returns
//     (see cSoftwareRenderer::retireDrainedMeshes), outside the seal.
//
// Memory ordering (one pair per cursor, named):
//  - tail_: producer stores with release AFTER writing the slot; consumer
//    loads with acquire BEFORE reading slots. Slot contents happen-before
//    the consumer's reads.
//  - head_: consumer stores with release AFTER moving slots out; producer
//    loads with acquire BEFORE reusing a slot. The consumer's reads of a
//    slot happen-before the producer's overwrite.
//  - consumedBytes_: consumer stores its running drained-byte total with
//    release; producer loads with acquire when applying the payload budget.
//  - pending_: the announce counter (design §6: "the announcer announces the
//    yield"). The producer increments it (acq_rel) BEFORE claiming a slot
//    and decrements it (acq_rel) AFTER the tail publish; the consumer reads
//    it (acquire) at the seal as the announcement that more may be in
//    flight beyond its drain target. Correctness never rests on it — the
//    cursors above carry that — it is the design's idiom made concrete.
//
// Backpressure policy (design §5): the ring budgets 8 MiB of payload
// (≈6 frames of worst-observed walk churn) across at most 512 slots.
// Ordinary replaces that would exceed the byte budget are refused (claim
// returns null; the caller keeps the mesh and retries later — the game's
// pending queue/spill is the holding place, so refusal is deferral, never
// loss). Must-push entries (tombstones, clears) ignore the byte budget but
// never the slot capacity. Every refusal is counted (pushFullCount) so a
// bench can prove the ring's headroom instead of assuming it.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "litholattice/render/pbr_quad_mesh.h"  // PbrChunkMesh
#include "litholattice/render/renderer.h"       // ChunkMesh

namespace litholattice {

class cSwUploadRing {
 public:
  static constexpr size_t kSlots = 512;                        // hard slot cap (power of two)
  static constexpr size_t kByteBudget = size_t(8) << 20;       // payload budget (design §5)
  static constexpr size_t kMask = kSlots - 1;
  static_assert((kSlots & kMask) == 0, "kSlots must be a power of two");

  enum class Kind : uint8_t { Chunk, PbrChunk, ClearAll };

  /// One staged upload. The producer fills it between claim() and publish();
  /// the consumer moves it out at drain and the slot returns empty.
  struct Slot {
    Kind kind = Kind::Chunk;
    uint32_t bytes = 0;      // payload bytes charged against kByteBudget
    ChunkMesh mesh;          // Kind::Chunk (empty verts = tombstone/remove)
    PbrChunkMesh pbrMesh;    // Kind::PbrChunk
  };

  // ---- Producer (main thread only) -------------------------------------
  /// Claim the next slot for an entry of `bytes` payload bytes, or return
  /// nullptr when the ring is full under the backpressure policy above.
  /// mustPush entries (tombstones, clears) skip the byte budget. On success
  /// the caller fills the slot and calls publish() exactly once.
  Slot* claim(uint32_t bytes, bool mustPush) {
    const uint64_t t = tailShadow_;
    uint64_t h = headCache_;
    if (t - h >= kSlots) {  // apparently full: refresh the cached consumer cursor once
      h = head_.load(std::memory_order_acquire);
      headCache_ = h;
    }
    if (t - h >= kSlots) {  // genuinely out of slots
      pushFull_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    if (!mustPush) {
      const uint64_t inFlight = pushedBytes_ - consumedBytes_.load(std::memory_order_acquire);
      if (inFlight + bytes > kByteBudget) {
        pushFull_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }
    }
    pending_.fetch_add(1, std::memory_order_acq_rel);  // announce BEFORE the slot write (design §6)
    Slot& s = slots_[t & kMask];
    s = Slot{};  // the consumer moved the previous occupant out at drain; reset the husk
    s.bytes = bytes;
    claimed_ = t;
    pushedBytes_ += bytes;
    const uint64_t occ = t + 1 - h;
    if (occ > maxOccupancy_.load(std::memory_order_relaxed))
      maxOccupancy_.store(occ, std::memory_order_relaxed);
    return &s;
  }

  /// Publish the claimed slot: the release store on tail_ is what makes the
  /// slot's contents visible to the consumer.
  void publish() {
    tail_.store(claimed_ + 1, std::memory_order_release);
    tailShadow_ = claimed_ + 1;
    pushed_.fetch_add(1, std::memory_order_relaxed);
    pending_.fetch_sub(1, std::memory_order_acq_rel);  // announcement complete: the push is visible
  }

  /// Total entries published so far — the seal snapshot a frame job records
  /// at submit (the executor drains up to exactly this index).
  uint64_t published() const { return tail_.load(std::memory_order_acquire); }
  /// Entries announced but not yet published (design §6 telemetry).
  uint64_t pendingAnnounce() const { return pending_.load(std::memory_order_acquire); }
  /// Entries published but not yet drained (as of the last cursor reads).
  uint64_t occupancyEstimate() const {
    const uint64_t t = tail_.load(std::memory_order_acquire);
    const uint64_t h = head_.load(std::memory_order_acquire);
    return t >= h ? t - h : 0;
  }

  // ---- Consumer (executor thread only) ---------------------------------
  /// Move every published entry with index < target out into `batch`
  /// (appended; slots are left empty). Returns the number drained. The
  /// release stores on head_/consumedBytes_ hand the slots and the payload
  /// budget back to the producer.
  size_t drainUpTo(uint64_t target, std::vector<Slot>& batch) {
    const uint64_t t = std::min(tail_.load(std::memory_order_acquire), target);
    const uint64_t h = headShadow_;
    if (t <= h) return 0;
    uint64_t bytes = 0;
    batch.reserve(batch.size() + size_t(t - h));
    for (uint64_t i = h; i < t; ++i) {
      Slot& s = slots_[i & kMask];
      bytes += s.bytes;
      batch.push_back(std::move(s));
    }
    headShadow_ = t;
    consumedBytesTotal_ += bytes;
    consumedBytes_.store(consumedBytesTotal_, std::memory_order_release);
    head_.store(t, std::memory_order_release);
    return size_t(t - h);
  }
  /// Entries drained so far (consumer's own cursor, mirrored for stats).
  uint64_t drainedTotal() const { return headShadow_; }

  // ---- Stats (relaxed atomics; read at shutdown) ------------------------
  uint64_t pushedCount() const { return pushed_.load(std::memory_order_relaxed); }
  uint64_t pushFullCount() const { return pushFull_.load(std::memory_order_relaxed); }
  uint64_t maxOccupancy() const { return maxOccupancy_.load(std::memory_order_relaxed); }

 private:
  Slot slots_[kSlots];
  // Producer-published cursor: written only by the producer, read by the consumer.
  alignas(64) std::atomic<uint64_t> tail_{0};
  // Consumer-published cursors: written only by the consumer, read by the producer.
  alignas(64) std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> consumedBytes_{0};
  // Announce counter (design §6): producer RMWs, consumer reads at the seal.
  alignas(64) std::atomic<uint64_t> pending_{0};
  // Stats (producer-written, relaxed).
  alignas(64) std::atomic<uint64_t> pushed_{0};
  std::atomic<uint64_t> pushFull_{0};
  std::atomic<uint64_t> maxOccupancy_{0};
  // Producer-only plain state (single-threaded by the SPSC contract).
  uint64_t tailShadow_ = 0;
  uint64_t headCache_ = 0;
  uint64_t pushedBytes_ = 0;
  uint64_t claimed_ = 0;
  // Consumer-only plain state.
  uint64_t headShadow_ = 0;
  uint64_t consumedBytesTotal_ = 0;
};

}  // namespace litholattice
