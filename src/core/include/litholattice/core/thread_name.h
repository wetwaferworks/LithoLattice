#pragma once
// Instrumentation only (JD t40u, 2026-11-16): name the calling OS thread so profilers,
// debuggers and the LL_SPIN_STATS readout can attribute CPU time per engine thread.
// Windows: SetThreadDescription. Linux: pthread_setname_np (15-char limit). Elsewhere a no-op.
// No behavior change.
#include <cstdint>
#include <cstdio>
#if defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void* hThread, const wchar_t* lpThreadDescription);
#elif defined(__linux__)
#  include <pthread.h>
#endif
#include "litholattice/core/spin_stats.h"

namespace litholattice {

inline void setCurrentThreadName(const char* name) {
  if (!name) return;
  uint32_t tid = 0;
#if defined(_WIN32)
  wchar_t w[64];
  int i = 0;
  for (; name[i] && i < 63; ++i) w[i] = wchar_t(static_cast<unsigned char>(name[i]));
  w[i] = 0;
  (void)SetThreadDescription(GetCurrentThread(), w);
  tid = uint32_t(GetCurrentThreadId());
#elif defined(__linux__)
  char buf[16];
  std::snprintf(buf, sizeof buf, "%s", name);
  pthread_setname_np(pthread_self(), buf);
#endif
  spinstats::setThreadName(name, tid);
}

}  // namespace litholattice
