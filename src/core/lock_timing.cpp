// Diagnostic-only main-thread lock-wait timing (0.0.1.60, Proposed by JD, Built by Cortana). See lock_timing.h.
#include "litholattice/core/lock_timing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "litholattice/core/log.h"

namespace litholattice::locktiming {
namespace {
thread_local bool tMain = false;
std::atomic<uint8_t> gHolder{kHolderNone};
// Main-thread-only accumulators (record() only runs on the marked main thread).
struct Frame { int64_t waitNs = 0, maxNs = 0; uint32_t acq = 0, contended = 0; int64_t site[kSiteCount] = {}; int64_t holder[kHolderCount] = {}; };
Frame gCur;
std::vector<double> gFrameWaitMs, gFrameMs;
int64_t gWinWait = 0, gWinMax = 0, gWinSite[kSiteCount] = {}, gWinHolder[kHolderCount] = {}, gLastFrameNs = 0;
uint64_t gWinAcq = 0, gWinCont = 0, gSiteAcq[kSiteCount] = {}, gSiteCont[kSiteCount] = {};
const char* const kSiteName[kSiteCount] = {"block", "handoff", "log", "arena", "jobpool"};
const char* const kHolderName[kHolderCount] = {"none", "simTick", "simPost", "other"};
double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}
}  // namespace

bool readEnabled() {
  const char* e = std::getenv("LL_LOCK_TIMING");
  return e && (std::strcmp(e, "1") == 0 || std::strcmp(e, "on") == 0 || std::strcmp(e, "true") == 0 || std::strcmp(e, "yes") == 0);
}
bool onMainThread() { return tMain; }
void markMainThread() {
  tMain = true;
  if (enabled()) logInfo("lock timing: ON (LL_LOCK_TIMING=1, diagnostic only; main-thread waits on block/handoff/log/arena/jobpool locks, summary every 120 frames)");
}
std::atomic<uint8_t>& blockHolder() { return gHolder; }
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void record(Site s, int64_t ns, Holder h, bool contended) {
  ++gCur.acq; ++gSiteAcq[s];
  if (!contended) return;
  ++gCur.contended; ++gSiteCont[s];
  gCur.waitNs += ns; gCur.maxNs = std::max(gCur.maxNs, ns); gCur.site[s] += ns; gCur.holder[h] += ns;
}

void frameEnd() {
  if (!enabled() || !tMain) return;
  const int64_t now = nowNs();
  if (gLastFrameNs) gFrameMs.push_back(double(now - gLastFrameNs) * 1e-6);
  gLastFrameNs = now;
  gFrameWaitMs.push_back(double(gCur.waitNs) * 1e-6);
  gWinWait += gCur.waitNs; gWinMax = std::max(gWinMax, gCur.maxNs); gWinAcq += gCur.acq; gWinCont += gCur.contended;
  for (int i = 0; i < kSiteCount; ++i) gWinSite[i] += gCur.site[i];
  for (int i = 0; i < kHolderCount; ++i) gWinHolder[i] += gCur.holder[i];
  gCur = Frame{};
  if (gFrameWaitMs.size() < 120) return;
  const double n = double(gFrameWaitMs.size());
  double frameSum = 0; for (double f : gFrameMs) frameSum += f;
  const double frameAvg = gFrameMs.empty() ? 0.0 : frameSum / double(gFrameMs.size());
  const double waitAvg = double(gWinWait) * 1e-6 / n;
  char sites[256], holders[160];
  int o = 0;
  for (int i = 0; i < kSiteCount; ++i)
    o += std::snprintf(sites + o, sizeof sites - size_t(o), "%s%s %.3f (%.1f acq, %.2f cont)", i ? ", " : "", kSiteName[i],
                       double(gWinSite[i]) * 1e-6 / n, double(gSiteAcq[i]) / n, double(gSiteCont[i]) / n);
  o = 0;
  for (int i = 1; i < kHolderCount; ++i)
    o += std::snprintf(holders + o, sizeof holders - size_t(o), "%s%s %.3f", i > 1 ? ", " : "", kHolderName[i], double(gWinHolder[i]) * 1e-6 / n);
  logInfo("lock timing: %d frames: main wait ms/frame avg %.3f p99 %.3f max %.3f, longest single wait %.3f ms, share %.1f%% of frame %.2f ms, "
          "acq/frame %.1f contended/frame %.2f | by lock ms/frame: %s | block holders ms/frame: %s",
          int(n), waitAvg, pct(gFrameWaitMs, 0.99), pct(gFrameWaitMs, 1.0), double(gWinMax) * 1e-6,
          frameAvg > 0 ? 100.0 * waitAvg / frameAvg : 0.0, frameAvg, double(gWinAcq) / n, double(gWinCont) / n, sites, holders);
  gFrameWaitMs.clear(); gFrameMs.clear();
  gWinWait = gWinMax = 0; gWinAcq = gWinCont = 0;
  for (int i = 0; i < kSiteCount; ++i) { gWinSite[i] = 0; gSiteAcq[i] = gSiteCont[i] = 0; }
  for (int i = 0; i < kHolderCount; ++i) gWinHolder[i] = 0;
}

}  // namespace litholattice::locktiming