/* setjmp.h ... LithoLibC's <setjmp.h>.
 *
 * On wasm32 the story is the compiler's SjLj lowering plus three
 * small runtime functions LithoLibC provides itself (src/libc/
 * setjmp.c, gated to wasm): clang's -wasm-enable-sjlj pass rewrites
 * setjmp / longjmp calls in a translation unit into calls to
 * __wasm_setjmp / __wasm_setjmp_test / __wasm_longjmp, and longjmp
 * unwinds as a wasm exception with the SjLj tag, so the whole
 * mechanism is in-module... no host import is involved and the
 * frozen LithoSys surface does not grow. The jmp_buf layout below
 * is exactly what those three functions use:
 *
 *   word 0: the func_invocation_id token the lowering passes
 *           (the address of a slot in the setjmp caller's frame;
 *           identity is pointer equality, per the lowering's
 *           contract)
 *   word 1: the label the lowering assigned to this setjmp site
 *           (never 0; the lowering's labels start at 1)
 *   word 2: the longjmp args slot: target env pointer
 *   word 3: the longjmp args slot: value (0 already mapped to 1)
 *
 * The args slot lives inside the jmp_buf because __wasm_longjmp
 * throws the address of that slot as the exception payload; the
 * catch glue the lowering emits reads the two words back out of it.
 *
 * Off wasm this header is not the sysroot's business: native builds
 * use their host C library's setjmp (LithoRoot is the wasm sysroot;
 * the Windows build never links LithoLibC). The include_next below
 * keeps host test builds that put this directory on their include
 * path working exactly as before this header existed.
 */
#ifndef LITHOLIBC_SETJMP_H
#define LITHOLIBC_SETJMP_H

#if defined(__wasm__)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lk_jmp_buf {
  unsigned int slot[4];
} jmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
}
#endif

#else /* !__wasm__ */

/* include_next is a GCC / Clang extension, and the host gates
 * build this library with -pedantic -Werror: the system_header
 * pragma (this file IS a system header when a host build includes
 * it) keeps the extension note out of the gate. MSVC has no
 * include_next and no need of one: native Windows builds use the
 * MSVC CRT and never put this sysroot in front of their standard
 * headers. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif
#include_next <setjmp.h>

#endif /* __wasm__ */

#endif /* LITHOLIBC_SETJMP_H */
