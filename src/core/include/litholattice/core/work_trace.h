#pragma once
// Duplicate-work trace (Direction JD, 2026-10-30): LL_WORK_TRACE=<file> records one event per
// pipeline stage (submit / mesh / land / drop) with a stable small thread index, the OS thread id,
// the sub-chunk key, the snapshot sequence and a steady-clock timestamp; workTraceFlush() writes
// them as CSV (stage,thread,os_tid,cx,sy,cz,seq,t_us). Off = one relaxed load per call.
#include <cstdint>
namespace litholattice {
bool workTraceOn();
void workTrace(const char* stage, int cx, int sy, int cz, uint64_t seq);
void workTraceFlush();
}  // namespace litholattice