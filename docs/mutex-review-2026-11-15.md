# Lock and mutex review (read only), 15 November 2026

Scope: the LithoLattice engine (`engine/`) and the LithoKraft game (`game/LithoKraft/`), after this
week's thread planner and GL fixes were folded in. No code was changed for this review. Line
numbers refer to the integration scratch at the time of writing.

The engine rule this is measured against: threads talk across phase boundaries; atomics (flags,
counters, lock-free rings, single-slot mailboxes) are fine; no locks or mutexes, and no thread
waits on another.

## Headline

* `std::mutex`, `std::recursive_mutex`, `std::shared_mutex` and `std::condition_variable` are
  gone from the desktop engine and game. The only real `std::mutex` left is in the web window
  backend (`window_emscripten.cpp`) and in the optional CEF host layer (`host/lithocef`).
* What replaced them is not lock-free. `cAtomicGate`, `cBlockGate` (reentrant) and the MCP
  `cSpinGate` are spinlocks built from an atomic bool: a second thread still waits for the
  first one, it just spins instead of sleeping. `cAtomicCondition` is a polling condition
  variable on top of a gate. By the "we don't wait on threads" rule these are still locks.
* Count in shipping code (tests and self-tests left out): **46 lock objects** (gates, spin
  gates, reentrant gates and real mutexes) plus **12 polling condition objects**, taken at
  roughly **240 guard sites**.
* Worst offenders, in order: `blockMutex_` in the sim thread, the block definition gate
  `defMutex_` (taken on every `blockDef()` read), the job pool view queue in `threading.h`,
  the Timing Handshake gate plus its two condition objects, the control stream inbound and
  inject gates (input to sim), and the global log gates.
* Proposed approach: three moves cover almost everything.
  1. **Tick-boundary ownership** for world data (sim owns columns during the tick; the main
     and stream threads get published snapshots between ticks). This retires `blockMutex_`.
  2. **Single-producer or multi-producer lock-free rings** for every "queue of work or events"
     gate (job views, I/O queue, control stream, storage, log).
  3. **Publish-once / double-buffer swap** for registries and caches that are written rarely
     and read constantly (block defs, resource caches, nav column cache, census telemetry).

## Inventory

Legend for the replacement column:
**PB** phase-boundary handoff at a tick edge, **DB** double buffer swapped at the tick boundary,
**SPSC/MPSC** lock-free ring, **AF** atomic flag or counter, **RCU** publish an immutable
snapshot through an atomic pointer (old copies freed at a later boundary), **SO** single owner
(only one thread ever touches it, so the gate can go and a debug owner check stays).

Risk: how likely the change is to move a hash or cause a bug. Effort: S under a day, M one to
three days, L a week or more.

### Sim and world (determinism-critical)

| # | Where | Primitive | Protects | Threads | Replacement | Risk | Effort |
|---|-------|-----------|----------|---------|-------------|------|--------|
| 1 | `engine/src/sim/include/litholattice/sim/sim_thread.h:379` (used in `sim_thread.cpp` 241-262, 770, 901, 928 and in `game.cpp` frameHold / stream insert) | `cBlockGate` (reentrant spin) | Live column pointers during the tick mutation window; `getBlock()` reads; stream column insert; staged promote; `runBetweenTicks` | sim/tick, main (raycast, `surfaceYAt`, stream pump), stream workers | **PB + RCU.** The sim owns live columns inside a tick. At the tick boundary it publishes a read view (the copy-on-write column table already exists behind `cowColumnsEnabled()`). Main-thread point reads go to the published view; column inserts become a request ring (MPSC) the sim drains at the start of its next tick. The existing `postBoundaryJob` single-slot mailbox becomes a small ring. | High (touches every world read; hashes must be re-checked across thread counts) | L |
| 2 | `engine/src/world/include/litholattice/world/block.h:399` (guards at 550, 570, 583, 625, 654, 669, 704, 713) | `cAtomicGate` (was `shared_mutex`) | `blockDefs_` growth versus `blockDef()` by-value reads | any reader thread (mesh workers, sim, main); writer is registration and `refreshStaged()` between ticks | **RCU.** Keep the def table in an immutable vector published through an atomic pointer and bumped `epoch_`; `refreshStaged()` builds a new vector and swaps it at the tick boundary; readers load the pointer once. Old tables freed after one boundary. | Medium | M |
| 3 | `engine/src/sim/include/litholattice/sim/timing_handshake.h:476-478` | `cAtomicGate m_` + `cAtomicCondition publishCv_`, `barrierCv_` | Shard epoch publish and arrival barrier (the "hybrid poll" path) | tick authority, shard workers | **AF.** The lock-free path already exists next to it (`arrivalWord_`, `publishSeq_`, `lfThreads_`). Make that the only path and delete the gate and both condition objects. | Medium (handshake is in the hash; re-run the hash matrix) | S-M |
| 4 | `timing_handshake.h:426` | `cAtomicGate statsM_` | `stats_` snapshot for other threads | tick authority writes, UI/MCP read | **AF seqlock** (the `statsSeq_` counter is already there): writer bumps odd/even, readers retry. | Low | S |
| 5 | `timing_handshake.h:468` | `cAtomicGate failureM_` | `pendingFailure_` record | any shard worker writes once, authority reads | **AF** first-writer-wins: an atomic state word plus a fixed record written once. | Low | S |
| 6 | `engine/src/sim/include/litholattice/sim/control_stream.h:163, 165` (`control_stream.cpp` 40, 49, 68, 72) | `cAtomicGate inboundM_`, `injectM_` | Inbound input events and injected (MCP/test) events before the sim stamps them | input/main and MCP threads produce, sim consumes | **MPSC ring** per source, drained by the sim at the start of the tick; the existing sort-by-stamp step stays on the sim side so ordering (and the hash) is unchanged. | Medium (input order feeds the hash; keep the stamp sort) | M |
| 7 | `control_stream.h:176` | `cAtomicGate recordM_` | Recording buffer | sim writes, exporter reads | **SPSC ring** or DB swapped at the tick boundary. | Low | S |
| 8 | `engine/src/world/include/litholattice/world/chunk.h:279` | `cAtomicGate` in a static dedupe table | Shared palette/entries dedupe map (`byHash`) | mesh/gen workers | **Per-thread tables** merged at a boundary, or a lock-free open-address table (`open_addr_table.h` exists). | Low | M |
| 9 | `engine/src/world/include/litholattice/world/chunk_ingress.h:185` | `cAtomicGate m_` | Overflow vector behind the MPSC ingress ring | gen workers produce, main consumes | Already has `cLfHandoff` overflow behind `LL_LF_INGRESS=1`. Make that the default and drop the gate. | Low | S |
| 10 | `engine/src/world/include/litholattice/world/chunk_swarm.h:30` | `cAtomicGate` | Slab free list | workers allocate, main frees | **Lock-free free list** (`slice_free_list.h` pattern) or per-thread caches. | Low | S-M |
| 11 | `engine/src/world/include/litholattice/world/neighbor_mesh.h:145` | `cAtomicGate stageMu_` | `staged_` pointers before the epoch flip | mesh workers stage, main flips | **MPSC ring** of staged pointers, flipped at the frame boundary (the slots are already atomics). | Low | S |

### Jobs, threads and I/O

| # | Where | Primitive | Protects | Threads | Replacement | Risk | Effort |
|---|-------|-----------|----------|---------|-------------|------|--------|
| 12 | `engine/src/core/include/litholattice/core/threading.h:395-397` | `cAtomicGate m_`, `cAtomicCondition cv_`, `idle_` | `cJobPool` view queues (`jobs_`, `urgent_`, `bg_`), `inFlight_`, `pumps_`; the old threaded path is now dead because the lock-free pool is always on | game thread submits; backing pool workers pump the view | **MPSC ring** per lane for the view queue, `inFlight_`/`pumps_` as atomics; `waitIdle` becomes a counter check the caller polls at a boundary instead of a wait. Remove the dead threaded path (`workers_`, `run()`). | Medium | M |
| 13 | `threading.h:479-480` | gate + condition | Fork-join batch counter (`pending_`, `failed_`) | workers decrement, caller waits | **AF** counters; the caller helps run jobs instead of waiting (same pattern as the lock-free pool's `waitIdle`). | Low | S |
| 14 | `threading.h:499` | `cAtomicGate m_` (timed guard `kHandoff`) | Handoff vector `items_` | producers push, owner drains | **MPSC ring** (`mpsc_queue.h` exists). | Low | S |
| 15 | `engine/src/core/include/litholattice/core/job_system.h:442, 447` | `cAtomicGate qGate_`, `ioGate_` | Priority deques and the I/O queue | any submitter, workers, I/O threads | **MPSC ring** per priority (or `lf_job_ring.h`); `ioWake_` already atomic. | Medium | M |
| 16 | `engine/src/render/software_renderer.cpp:538` | `cAtomicGate runGate_` | One band-job dispatch at a time | render caller vs band workers | **AF** generation + `remaining_` counter (already atomic); the gate only serializes callers, so make the renderer single-owner and drop it. | Low | S |
| 17 | `engine/src/storage/include/litholattice/storage/storage.h:199-200` (`storage.cpp` 364-505) | gate + `cv_`, `idle_` | Storage job deque, `busy_`, `pumping_` | game submits, storage I/O thread runs | **SPSC ring** to the I/O thread plus an atomic idle counter; flush waits become a boundary check. | Low-Medium | M |
| 18 | `engine/src/storage/include/litholattice/storage/async_io.h:85-86` | gate + condition | I/O fence (`pending_`, `failed_`) | I/O completes, caller waits | **AF** counters, caller polls at a boundary. | Low | S |
| 19 | `async_io.h:149-150` | gate + `drainCv_` | Per-path strands map, `inflight_` | submitters, job system | **Single owner**: route all strand bookkeeping through one I/O coordinator thread fed by an MPSC ring. | Medium | M |
| 20 | `engine/src/core/include/litholattice/core/thread_census.h:229` | `cAtomicGate m_` | `slots_` growth and telemetry (hot path already atomic) | any thread registering | **Fixed slot array** sized at boot (max threads is known) so growth never happens. | Low | S |
| 21 | `engine/src/ai/ai_world.cpp:177-178` | local gate + condition | Fork-join path search countdown | pool workers, caller | **AF** countdown; caller runs remaining jobs inline (`onAnyWorker` pattern). | Low | S |

### Render, assets, platform

| # | Where | Primitive | Protects | Threads | Replacement | Risk | Effort |
|---|-------|-----------|----------|---------|-------------|------|--------|
| 22 | `game/LithoKraft/src/app/main.cpp:607-608` | gate + condition | Render-thread mailbox (`has`, `busy`, `quit`, `job`) | main, render thread | **Single-slot atomic mailbox** (two buffers + an atomic index), the same shape as `boundaryJob_`. | Low | S |
| 23 | `main.cpp:691-692` | gate + condition | Software render job mailbox (`job`, `jobActive`, `jobErr`) | main, software render executor | Same single-slot mailbox. | Low | S |
| 24 | `engine/src/render_vk/vk_vertex_arena.h:96` | `cAtomicGate mu_` | Arena block table growth | upload workers, render | Fixed table already sized (`kMaxBlocks`); make growth an atomic bump of `count_`. | Low | S |
| 25 | `engine/src/render_vk/include/litholattice/render_vk/packed_quad64.h:90` | `cAtomicGate mu` | Diagnostic sub-cell census maps (off by default) | mesh workers | Per-thread maps merged at exit. | Low | S |
| 26 | `engine/src/assets/include/litholattice/assets/resource_loader.h:80, 127` (11 guards in `resource_loader.cpp`) | `cAtomicGate mutex_` (x2) | Image and blob LRU caches | loader workers, main | **RCU** cache snapshot or single owner (loader thread) with an MPSC request ring. | Low | M |
| 27 | `game/LithoKraft/src/game/include/lithokraft/game/resource_manager.h:85` | `cAtomicGate mutex_` | Materials, icons, theme | load thread, main | **RCU**: publish `shared_ptr<const ...>` atomically (materials already are). | Low | S |
| 28 | `engine/src/ai/include/litholattice/ai/nav.h:160` | `cAtomicGate m_` | Nav column cache and LRU | path jobs | **RCU** per column + generation counter (already present). | Low | M |
| 29 | `engine/src/audio/include/litholattice/audio/occlusion_thread.h:98` | `cAtomicGate queriesMu_` | Query list growth | game adds, occlusion thread snapshots | **MPSC ring** of new queries; occlusion thread owns the list. | Low | S |
| 30 | `engine/src/core/include/litholattice/core/f3_trace.h:20` | `cAtomicGate m` | Debug HUD text sample | renderer writes, test reads | **DB** with atomic index. | Low | S |
| 31 | `engine/src/platform/window_web.cpp:72, 730` | `cAtomicGate` (x2) | Web input queue and conversion | browser thread, drain thread | **SPSC ring** (the ring path already exists; retire the legacy drain). | Low | S |
| 32 | `engine/src/platform/window_emscripten.cpp:65, 863` | **`std::mutex`** (x2) | Same as above on the Emscripten build | browser thread, drain thread | Same as 31; this is the last `std::mutex` in the engine proper. | Low | S |

### Logging, tracing, MCP, game helpers

| # | Where | Primitive | Protects | Threads | Replacement | Risk | Effort |
|---|-------|-----------|----------|---------|-------------|------|--------|
| 33 | `engine/src/core/log.cpp:20` | `cAtomicGate gLogMutex` | Log file writes | every thread | **MPSC ring** to one log writer thread (or per-thread rings flushed by main). | Low | S-M |
| 34 | `engine/src/core/include/litholattice/core/log.h:56` | `cAtomicGate mutex_` | In-memory log ring for the console | every thread writes, UI reads | Lock-free ring with a sequence number per record. | Low | S |
| 35 | `engine/src/core/thread_debug.cpp:28` | `cAtomicGate g_threadLogMutex` | Debug line output | every thread (debug only) | Already has per-thread trace rings; route output through them. | Low | S |
| 36 | `engine/src/core/work_trace.cpp:16` | `cAtomicGate g_mx` | Buffer registration (once per thread) | worker threads | Fixed array + atomic index (`g_next` exists). | Low | S |
| 37 | `engine/src/mcp/include/litholattice/mcp/http_server.h:228` (18 guards in `http_server.cpp`) | `cSpinGate mutex_` | MCP session map | HTTP accept/IO threads | **Single owner**: one MCP owner thread holds sessions, IO threads hand requests over an MPSC ring (the pilot controller already moved to this model). | Medium | M |
| 38 | `game/LithoKraft/src/game/camera_controller.cpp:176` | `cAtomicGate mutex` | Pilot camera map | MCP owner, main | **SO** (MCP owner) + published pose snapshot per frame. | Low | S |
| 39 | `game/LithoKraft/src/game/include/lithokraft/game/mcp_senses_adapter.h:173` | `cAtomicGate mutex_` | Cached meshes, cvars, sprite verts | main publishes, MCP owner reads | **RCU** per frame. | Low | S |
| 40 | `game/LithoKraft/src/game/include/lithokraft/game/bench_stream.h:83` | `cAtomicGate mapMu_` (shared) | Bench streamer column map | bench workers, main | Bench only; MPSC ring of finished columns. | Low | S |
| 41 | `game/LithoKraft/src/app/perf_headless.cpp:85` | `cAtomicGate m_` | Tick timing samples | sim thread writes, harness reads | SPSC ring. | Low | S |

### Outside the engine proper (noted, not proposed for this pass)

* `host/lithocef/thread_handshake.cpp:44-45`: `std::mutex` + `std::condition_variable` in the
  optional CEF host layer (built only with `LL_WITH_LITHOCEF=ON`). Same MPSC-ring fix applies
  if that layer comes back into play.
* `engine/lithoroot/`: the freestanding runtime implements `pthread_mutex` and friends for
  ported code (`lithothread.c`, `lithoweave_native.c`, `include/pthread.h`). That is a platform
  layer, not engine logic; leave it.
* `engine/src/core/include/litholattice/core/lock_timing.h`: the `TimedGuard` wrapper only
  measures gate waits. It goes away on its own as the gates go.
* Tests and self-tests use gates to check ordering; they can keep them until the engine side
  is clean.

## Suggested order

1. Quick wins, no hash risk (about a day): 4, 5, 7, 9, 13, 14, 16, 18, 20, 21, 22, 23, 24, 30, 36.
2. Queues to rings (two to three days): 6, 11, 12, 15, 17, 29, 33, 34. Re-run the hash matrix
   after 6 and 12.
3. Publish-once registries and caches (two to three days): 2, 26, 27, 28, 39.
4. The big one: 1 (`blockMutex_`), with 3 folded in. Plan it as its own week. It needs the
   copy-on-write column table on by default and a request ring for stream inserts; the
   place-lag work from earlier this month (edits no longer forcing a full pass) already
   shrank how long the gate is held, which makes the cut-over safer.
5. Finish with 31, 32, 37, 38, 40, 41 and delete `atomic_gate.h`, `block_gate.h`,
   `gate_lock.h`, `atomic_condition.h` and `spin_gate.h` once nothing includes them.

Every step should end with the usual checks: self-test against the known baseline and the
world and sim hashes compared across 4 and 12 threads. Any hash that moves gets written down
with its cause.
