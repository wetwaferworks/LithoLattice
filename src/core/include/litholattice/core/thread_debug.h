// Verbose thread/job-system debug logging, compile-flag-gated.
//
// Define LITHOLATTICE_THREAD_DEBUG (CMake option LITHOLATTICE_THREAD_DEBUG, off by
// default) to instrument the threading machinery: job lifecycle (submit / steal /
// start / finish + duration tripwire), lock acquire/release with the documented lock
// order, thread wake/sleep transitions, per-deque depths, thread naming and affinity
// assignments, and input-pump queue behavior.
//
// When the flag is OFF every macro below expands to nothing: zero overhead, no
// runtime branches in hot paths (verified: the instrumented self-tests build and pass
// both ways). B8 (2026-10-23): when ON, every line goes to the calling thread's lock-free
// trace ring (256 lines, single writer, no lock); LITHO_THREAD_DUMP(why) prints all rings,
// and the job-system waits dump them after 10 s without progress (watchdog, ON builds only;
// the wait itself is unchanged). Set LITHO_THREAD_LOG_STDERR=1 to also echo each line live
// to stderr via a dedicated leaf lock (see the lock
// order in job_system.h: the debug-log lock is L3, the absolute leaf — it is only
// ever taken to emit one line and never held across other work).
#pragma once

#include <cstdint>

namespace litholattice {
namespace detail {

#ifdef LITHOLATTICE_THREAD_DEBUG
/// Emit one debug line. `tag` selects the subsystem ("job", "lock", "thread", "input").
/// Cheap: guarded by the compile-time flag at every call site, so disabled builds pay
/// nothing. The sink itself takes the leaf debug-log lock and releases it per line.
void threadLog(const char* tag, const char* fmt, ...);
#endif
/// Monotonic microsecond clock for debug timestamps. Declared unconditionally so
/// `if constexpr (kThreadDebug)` call sites compile both ways; defined (and only
/// ever called) when LITHOLATTICE_THREAD_DEBUG is on.
uint64_t threadDebugNowUs();
/// Next job id for lifecycle tracing. Same unconditional-declaration rule.
uint64_t threadDebugNextJobId();
#ifdef LITHOLATTICE_THREAD_DEBUG
/// B8 (2026-10-23): print every thread's trace ring (newest last) to stderr, headed by `why`.
void threadDebugDump(const char* why);
#endif

}  // namespace detail
}  // namespace litholattice

#ifdef LITHOLATTICE_THREAD_DEBUG
#define LITHO_THREAD_LOG(tag, ...) \
  ::litholattice::detail::threadLog((tag), __VA_ARGS__)
#define LITHO_THREAD_DUMP(why) ::litholattice::detail::threadDebugDump(why)
#else
// Zero-overhead when disabled: no code, no branch, arguments never evaluated.
#define LITHO_THREAD_LOG(tag, ...) ((void)0)
#define LITHO_THREAD_DUMP(why) ((void)0)
#endif
