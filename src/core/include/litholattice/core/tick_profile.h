#pragma once
// Measure-only tick / render profile (LL_TICK_PROFILE=1, default OFF). Timestamps and log lines only; it never
// changes what the sim or the renderer do. Proposed by JD; co-implemented by JD and Cortana.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "litholattice/core/log.h"
namespace litholattice::tickprof {
inline bool enabled() {
  static const bool on = [] { const char* e = std::getenv("LL_TICK_PROFILE"); return e && e[0] == '1'; }();
  return on;
}
inline int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline double ms(int64_t ns) { return double(ns) * 1e-6; }

/// World-simulation sub-phases of the current tick (sim thread only; written by cSignalWorldSim::tick).
struct WorldSub { int64_t signalNs = 0, romNs = 0; };
inline WorldSub& worldSub() { static WorldSub w; return w; }

enum Tick { kDom, kCtrl, kSignal, kRom, kWorldOther, kSimRandom, kRandomTicks, kTickCount };
/// Per-second tick breakdown (sim thread only). "Long" = world tick over 10 ms (domain phase excluded).
struct TickAgg {
  int64_t sum[kTickCount] = {}, longSum[kTickCount] = {}, worst[kTickCount] = {};
  int64_t worstTotal = 0, longTotal = 0, flushMax = 0, flushSum = 0, t0 = 0;
  uint64_t n = 0, nLong = 0, flushes = 0, bursts = 0, burstSteps = 0;
  int maxBurst = 0;
  void noteBurst(int steps) { maxBurst = std::max(maxBurst, steps); ++bursts; burstSteps += uint64_t(steps); }
  void noteFlush(int64_t ns) { flushMax = std::max(flushMax, ns); flushSum += ns; ++flushes; }
  void add(const int64_t* ph) {
    int64_t tot = 0;
    for (int i = kCtrl; i < kTickCount; ++i) tot += ph[i];
    for (int i = 0; i < kTickCount; ++i) sum[i] += ph[i];
    ++n;
    if (tot > 10'000'000) { ++nLong; longTotal += tot; for (int i = 0; i < kTickCount; ++i) longSum[i] += ph[i]; }
    if (tot > worstTotal) { worstTotal = tot; for (int i = 0; i < kTickCount; ++i) worst[i] = ph[i]; }
    const int64_t now = nowNs();
    if (!t0) t0 = now;
    if (now - t0 < 1'000'000'000) return;
    static const char* const kName[kTickCount] = {"domains", "control", "signal", "patternROM", "worldOther", "simRandom", "randomTicks"};
    char b[2048];
    int o = std::snprintf(b, sizeof b, "tick profile: %llu ticks, %llu long | avg ms:", (unsigned long long)n, (unsigned long long)nLong);
    for (int i = 0; i < kTickCount; ++i) o += std::snprintf(b + o, sizeof b - size_t(o), " %s %.3f", kName[i], ms(sum[i]) / double(n));
    o += std::snprintf(b + o, sizeof b - size_t(o), " | long ms:");
    for (int i = 0; i < kTickCount; ++i) o += std::snprintf(b + o, sizeof b - size_t(o), " %s %.3f", kName[i], ms(longSum[i]));
    o += std::snprintf(b + o, sizeof b - size_t(o), " | worst %.2f:", ms(worstTotal));
    for (int i = 0; i < kTickCount; ++i) o += std::snprintf(b + o, sizeof b - size_t(o), " %s %.2f", kName[i], ms(worst[i]));
    std::snprintf(b + o, sizeof b - size_t(o), " | bursts %llu steps %llu max %d | flush n %llu sum %.2f max %.2f",
                  (unsigned long long)bursts, (unsigned long long)burstSteps, maxBurst, (unsigned long long)flushes, ms(flushSum), ms(flushMax));
    logInfo("%s", b);
    *this = TickAgg{};
    t0 = now;
  }
};
inline TickAgg& ticks() { static TickAgg a; return a; }

/// 2026-11-09 next-build-2 (GUI diagnosis Section 6): HUD expansion time
/// (buildHudVertices) of the most recent frame, written by whichever
/// backend expanded the HUD (render thread in threeway, main otherwise).
/// Plain value: the app loop's spike record reads it one frame late at
/// worst, which the spike line's own wording allows for.
inline int64_t& hudExpandNs() { static int64_t v = 0; return v; }

enum RPhase { kSprite, kWaitFence, kHousekeep, kAcquire, kRecord, kSubmit, kPresent, kRCount };
/// Vulkan renderFrame phases of the last frame (render thread only).
struct RenderPh { int64_t ns[kRCount] = {}; bool valid = false; };
inline RenderPh& render() { static RenderPh r; return r; }
/// Render-thread frame breakdown, logged every 240 frames. copy/handoff are added by the main thread.
struct FrameAgg {
  std::atomic<int64_t> copyNs{0}, handoffNs{0};
  std::atomic<uint64_t> copies{0}, handoffs{0};
  static constexpr int kItems = 3 + kRCount;
  int64_t sum[kItems] = {}, mx[kItems] = {};
  uint64_t n = 0, nValid = 0;
  void add(int64_t lockWait, int64_t renderFrame, int64_t hook) {
    int64_t v[kItems] = {lockWait, renderFrame, hook};
    RenderPh& rp = render();
    if (rp.valid) { ++nValid; for (int i = 0; i < kRCount; ++i) v[3 + i] = rp.ns[i]; }
    rp.valid = false;
    for (int i = 0; i < kItems; ++i) { sum[i] += v[i]; mx[i] = std::max(mx[i], v[i]); }
    if (++n < 240) return;
    static const char* const kName[kItems] = {"lockWait", "renderFrame", "shotHook", "vk.sprite", "vk.waitFence", "vk.housekeep", "vk.acquire", "vk.record", "vk.submit", "vk.present"};
    char b[2048];
    int o = std::snprintf(b, sizeof b, "render profile: %llu frames (vk %llu) avg/max ms:", (unsigned long long)n, (unsigned long long)nValid);
    for (int i = 0; i < kItems; ++i)
      o += std::snprintf(b + o, sizeof b - size_t(o), " %s %.3f/%.2f", kName[i], ms(sum[i]) / double(i < 3 ? n : std::max<uint64_t>(1, nValid)), ms(mx[i]));
    const uint64_t c = copies.exchange(0), h = handoffs.exchange(0);
    const int64_t cn = copyNs.exchange(0), hn = handoffNs.exchange(0);
    std::snprintf(b + o, sizeof b - size_t(o), " | main handoff wait avg %.3f ms, frame copy avg %.3f ms",
                  h ? ms(hn) / double(h) : 0.0, c ? ms(cn) / double(c) : 0.0);
    logInfo("%s", b);
    for (int i = 0; i < kItems; ++i) sum[i] = mx[i] = 0;
    n = nValid = 0;
  }
};
inline FrameAgg& frames() { static FrameAgg a; return a; }

/// Tick ledger overhead (Thread Handshake P4; authority thread only): bookkeeping ns per
/// tick (open + barrier sweep, async wait excluded), logged once a second.
struct LedgerAgg {
  int64_t sum = 0, mx = 0, t0 = 0;
  uint64_t n = 0, slots = 0, asyncs = 0;
  void add(uint64_t ns, uint32_t k, uint64_t a) {
    sum += int64_t(ns); mx = std::max(mx, int64_t(ns)); ++n; slots += k; asyncs += a;
    const int64_t now = nowNs();
    if (!t0) t0 = now;
    if (now - t0 < 1'000'000'000) return;
    logInfo("tick profile: ledger %llu ticks, %llu sync / %llu async records, overhead avg %.4f ms max %.4f ms per tick",
            (unsigned long long)n, (unsigned long long)slots, (unsigned long long)asyncs, ms(sum) / double(n), ms(mx));
    *this = LedgerAgg{};
    t0 = now;
  }
};
inline LedgerAgg& ledger() { static LedgerAgg a; return a; }
}  // namespace litholattice::tickprof
