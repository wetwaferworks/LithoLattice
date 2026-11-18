/* assert.h ... LithoLibC (S2 L1, 2026-11-16). No include guard: the
 * standard requires assert to follow the latest NDEBUG at each include.
 * A failed assert writes "file:line: func: Assertion `expr' failed."
 * through lithosys_console_write (safe before libc stdio is up, from any
 * thread) and calls lithosys_abort. No locks, no allocation. */
#if !defined(__wasm__)
#error "LithoRoot std headers are wasm32-only; native builds use their own C++ library"
#endif
#include <lithosys.h>
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#ifndef LITHOLIBC_ASSERT_FAIL_DEFINED
#define LITHOLIBC_ASSERT_FAIL_DEFINED
#ifdef __cplusplus
extern "C" {
#endif
static inline uint32_t __litholibc_assert_len(const char* s) { uint32_t n = 0; while (s[n]) ++n; return n; }
static inline void __litholibc_assert_put(const char* s) { lithosys_console_write(s, __litholibc_assert_len(s)); }
__attribute__((__noreturn__)) static inline void __litholibc_assert_fail(const char* expr, const char* file, unsigned line, const char* func) {
  char num[12]; int i = 11; num[i] = 0;
  do { num[--i] = (char)('0' + line % 10); line /= 10; } while (line && i > 0);
  __litholibc_assert_put(file); __litholibc_assert_put(":"); __litholibc_assert_put(num + i);
  __litholibc_assert_put(": "); __litholibc_assert_put(func); __litholibc_assert_put(": Assertion `");
  __litholibc_assert_put(expr); __litholibc_assert_put("' failed.\n");
  lithosys_abort();
}
#ifdef __cplusplus
}
#endif
#endif
#define assert(e) ((e) ? (void)0 : __litholibc_assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
