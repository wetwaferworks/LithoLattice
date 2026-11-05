#include "litholattice/world/dimension.h"
#include "litholattice/core/log.h"
#include "litholattice/core/lf_job_queue.h"  // 2026-11-01 LL_LF_WORLDBUILD
#include <algorithm>
#include <atomic>
#include <vector>

namespace litholattice {

Spawn Dimension::load(cJobPool& pool, int sizeChunks) {
  unload();
  cHandoffQueue<std::unique_ptr<ChunkColumn>> out;
  int failed = 0;
  if (!generator) {
    logError("dimension '%s': no terrain generator; loading empty", id.c_str());
  } else if (const char* e = std::getenv("LL_LF_WORLDBUILD"); !(e && e[0] == '0') && cLfJobQueue::enabled()) {  // PlayL: default ON (JD 2026-11-01)
    // 2026-11-01 (JD LL_LF_WORLDBUILD=1 + LL_LF_JOBS=1): every column job writes only its own
    // pre-sized slot (atomic handoff via the pending counter's release/acquire), then the
    // columns are inserted in fixed (cz, cx) order: deterministic, no mutex, no condvar.
    const std::shared_ptr<const cColumnGen> gen = generator;
    const size_t n = size_t(sizeChunks) * size_t(sizeChunks);
    std::vector<std::unique_ptr<ChunkColumn>> slots(n);
    std::vector<uint8_t> bad(n, 0);
    std::atomic<int> pending{int(n)};
    cLfJobQueue& q = cLfJobQueue::shared();
    for (int cz = 0; cz < sizeChunks; ++cz)
      for (int cx = 0; cx < sizeChunks; ++cx) {
        const size_t i = size_t(cz) * size_t(sizeChunks) + size_t(cx);
        q.submit([&slots, &bad, &pending, gen, cx, cz, i] {
          try { slots[i] = gen->generate(cx, cz); if (!slots[i]) bad[i] = 1; }
          catch (const std::exception& ex) { bad[i] = 1; logError("world build: column %d,%d threw: %s", cx, cz, ex.what()); }
          catch (...) { bad[i] = 1; logError("world build: column %d,%d threw", cx, cz); }
          pending.fetch_sub(1, std::memory_order_release);
        });
      }
    q.helpUntilZero(pending);  // acquire: every slot write is visible after this
    for (size_t i = 0; i < n; ++i) {
      if (slots[i]) chunks.insert(std::move(slots[i]));
      failed += bad[i];  // matches JobBatch: a generate() that throws counts as failed
    }
  } else {
    const std::shared_ptr<const cColumnGen> gen = generator;
    JobBatch batch(pool);
    for (int cz = 0; cz < sizeChunks; ++cz)
      for (int cx = 0; cx < sizeChunks; ++cx)
        batch.submit([&out, gen, cx, cz] {
          if (auto col = gen->generate(cx, cz)) out.push(std::move(col));
        });
    failed = batch.wait();  // blocks on a condition variable; failed columns are logged and skipped
  }
  std::vector<std::unique_ptr<ChunkColumn>> cols;
  out.drainInto(cols);
  for (auto& c : cols) chunks.insert(std::move(c));
  if (failed) logError("dimension '%s': %d of %d columns failed to generate (left empty)", id.c_str(), failed, sizeChunks * sizeChunks);
  lastLoadFailures = failed;
  const Spawn sp = setup ? setup(chunks, sizeChunks) : Spawn{8.f, 100.f, 8.f, 0.785f, -0.3f};
  loaded = true;
  return sp;
}

Spawn Dimension::loadRadius(cJobPool& pool, int sizeChunks, int ocx, int ocz, int radius) {
  // Extent for setup/spawn stays sizeChunks; only the near disk is generated now. The streamer
  // (and generateMissing for authored content) fills the rest. Same LF path as load().
  unload();
  if (sizeChunks < 1) sizeChunks = 1;
  if (radius < 0) radius = 0;
  const int cx0 = std::max(0, ocx - radius);
  const int cz0 = std::max(0, ocz - radius);
  const int cx1 = std::min(sizeChunks, ocx + radius + 1);
  const int cz1 = std::min(sizeChunks, ocz + radius + 1);
  lastLoadFailures = 0;
  generateMissing(pool, cx0, cz0, cx1, cz1);  // updates lastLoadFailures on failure
  const Spawn sp = setup ? setup(chunks, sizeChunks) : Spawn{8.f, 100.f, 8.f, 0.785f, -0.3f};
  loaded = true;
  return sp;
}

int Dimension::generateMissing(cJobPool& pool, int cx0, int cz0, int cx1, int cz1) {
  if (!generator) {
    logError("dimension '%s': no terrain generator; generateMissing is a no-op", id.c_str());
    return 0;
  }
  if (cx1 <= cx0 || cz1 <= cz0) return 0;
  struct Want { int cx, cz; size_t i; };
  std::vector<Want> want;
  want.reserve(size_t(std::max(0, cx1 - cx0)) * size_t(std::max(0, cz1 - cz0)));
  for (int cz = cz0; cz < cz1; ++cz)
    for (int cx = cx0; cx < cx1; ++cx)
      if (!chunks.contains(cx, cz)) want.push_back({cx, cz, want.size()});
  if (want.empty()) return 0;
  const std::shared_ptr<const cColumnGen> gen = generator;
  int failed = 0;
  int generated = 0;
  if (const char* e = std::getenv("LL_LF_WORLDBUILD"); !(e && e[0] == '0') && cLfJobQueue::enabled()) {
    std::vector<std::unique_ptr<ChunkColumn>> slots(want.size());
    std::vector<uint8_t> bad(want.size(), 0);
    std::atomic<int> pending{int(want.size())};
    cLfJobQueue& q = cLfJobQueue::shared();
    for (const Want& w : want) {
      q.submit([&slots, &bad, &pending, gen, w] {
        try { slots[w.i] = gen->generate(w.cx, w.cz); if (!slots[w.i]) bad[w.i] = 1; }
        catch (const std::exception& ex) { bad[w.i] = 1; logError("world build: column %d,%d threw: %s", w.cx, w.cz, ex.what()); }
        catch (...) { bad[w.i] = 1; logError("world build: column %d,%d threw", w.cx, w.cz); }
        pending.fetch_sub(1, std::memory_order_release);
      });
    }
    q.helpUntilZero(pending);
    for (size_t i = 0; i < want.size(); ++i) {
      if (slots[i]) { chunks.insert(std::move(slots[i])); ++generated; }
      failed += bad[i];
    }
  } else {
    cHandoffQueue<std::unique_ptr<ChunkColumn>> out;
    JobBatch batch(pool);
    for (const Want& w : want)
      batch.submit([&out, gen, cx = w.cx, cz = w.cz] {
        if (auto col = gen->generate(cx, cz)) out.push(std::move(col));
      });
    failed = batch.wait();
    std::vector<std::unique_ptr<ChunkColumn>> cols;
    out.drainInto(cols);
    for (auto& c : cols) { chunks.insert(std::move(c)); ++generated; }
  }
  if (failed) logError("dimension '%s': %d of %d missing columns failed to generate", id.c_str(), failed, int(want.size()));
  lastLoadFailures += failed;
  return generated;
}

}  // namespace litholattice
