// Deterministic parallelism scheduler: conflict-DAG stage building,
// per-stage parallel dispatch on a cJobPool, and ordered command-buffer drains.
#include "litholattice/sim/det_sched.h"

#include "litholattice/core/bitops.h"

#include <utility>

namespace litholattice {

uint64_t detMix64(uint64_t x) {
  // splitmix64 (public domain, Steele et al.): well-defined unsigned overflow.
  // Build 59: spelled through the shared bit-ops header; bit-exact with
  // the original body (tools/bitops_diff_test.cpp).
  return bitops::splitmixMix64(x + 0x9E3779B97F4A7C15ULL);
}

uint64_t detKey(uint64_t entity, uint64_t tick, uint64_t salt) {
  // Chain the three inputs through splitmix; order is fixed and every bit of
  // every input avalanches, so (entity, tick, salt) triples never collide
  // short of a 64-bit birthday hit.
  uint64_t h = detMix64(entity + 0x9E3779B97F4A7C15ULL);
  h = detMix64(h ^ (tick + 0xBF58476D1CE4E5B9ULL));
  h = detMix64(h ^ (salt + 0x94D049BB133111EBULL));
  return h;
}

uint64_t cDeterministicScheduler::hashBytes(const void* p, size_t n, uint64_t h) {
  const auto* b = static_cast<const unsigned char*>(p);
  // Build 59: the step is spelled through the shared bit-ops header
  // (kFnvPrime == 0x100000001B3, proven in tools/bitops_diff_test.cpp).
  // The loop itself is never re-associated — order is the hash format.
  for (size_t i = 0; i < n; ++i) h = bitops::fnv1aStep(h, b[i]);
  return h;
}

size_t cDeterministicScheduler::addSystem(std::unique_ptr<ISimSystem> sys) {
  const size_t id = systems_.size();
  systems_.push_back(std::move(sys));
  buffers_.push_back(std::make_unique<CommandBuffer>());
  rebuildStages();
  return id;
}

namespace {

// Two systems conflict on a resource unless both only read it.
bool conflictsOn(const std::vector<ResourceAccess>& a, const std::vector<ResourceAccess>& b) {
  for (const ResourceAccess& x : a)
    for (const ResourceAccess& y : b)
      if (x.resource == y.resource && (x.write || y.write)) return true;
  return false;
}

}  // namespace

void cDeterministicScheduler::rebuildStages() {
  const size_t n = systems_.size();
  std::vector<std::vector<ResourceAccess>> decl(n);
  for (size_t i = 0; i < n; ++i) decl[i] = systems_[i]->access();
  // Greedy list scheduling in registration order: a system lands one stage
  // past the latest earlier system it conflicts with. Deterministic for a
  // fixed registration order; conservative (read/write and write/write both
  // conflict) so systems in one stage provably write disjoint data.
  std::vector<size_t> stageOf(n, 0);
  size_t stageCount = 0;
  for (size_t i = 0; i < n; ++i) {
    size_t s = 0;
    for (size_t j = 0; j < i; ++j)
      if (conflictsOn(decl[i], decl[j]) && stageOf[j] + 1 > s) s = stageOf[j] + 1;
    stageOf[i] = s;
    if (s + 1 > stageCount) stageCount = s + 1;
  }
  stages_.assign(stageCount, {});
  for (size_t i = 0; i < n; ++i) stages_[stageOf[i]].push_back(i);
}

void cDeterministicScheduler::drainStage(void* world) {
  // Fixed deterministic order: system registration order, then per-system seq.
  // Single-threaded here; commands become visible to the next stage, never
  // mid-stage.
  for (size_t id = 0; id < buffers_.size(); ++id) {
    if (buffers_[id]->empty()) continue;
    drainTmp_.clear();
    buffers_[id]->drainInto(drainTmp_);
    for (const StructuralCommand& cmd : drainTmp_) {
      if (cmd.kind == StructuralCommand::Kind::Custom && cmd.apply) {
        cmd.apply(world);
      } else {
        apply_(world, id, cmd);
      }
    }
  }
  drainTmp_.clear();
}

void cDeterministicScheduler::runTick(uint64_t tick, void* world, cJobPool& pool) {
  const uint64_t failedBefore = pool.failedJobs();
  for (size_t s = 0; s < stages_.size(); ++s) {
    for (const size_t id : stages_[s]) {
      ISimSystem* sys = systems_[id].get();
      CommandBuffer* buf = buffers_[id].get();
      pool.submit([sys, tick, world, buf] {
        SchedTickContext ctx{tick, world, *buf};
        sys->update(ctx);
      });
    }
    pool.waitIdle();
    drainStage(world);
    if (pool.failedJobs() != failedBefore) {
      throw std::runtime_error("det-sched: a system job threw; tick aborted (world may be half-updated)");
    }
  }
}

void cDeterministicScheduler::runTick(uint64_t tick, void* world, cJobSystem& jobs) {
  for (size_t s = 0; s < stages_.size(); ++s) {
    JobFence fence(jobs);  // the stage barrier: all systems done before the drain
    for (const size_t id : stages_[s]) {
      ISimSystem* sys = systems_[id].get();
      CommandBuffer* buf = buffers_[id].get();
      fence.submit([sys, tick, world, buf] {
        SchedTickContext ctx{tick, world, *buf};
        sys->update(ctx);
      });
    }
    const int failed = fence.wait();
    drainStage(world);
    if (failed != 0) {
      throw std::runtime_error("det-sched: a system job threw; tick aborted (world may be half-updated)");
    }
  }
}

}  // namespace litholattice
