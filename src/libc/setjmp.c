/* setjmp.c ... the wasm SjLj runtime: __wasm_setjmp,
 * __wasm_setjmp_test, __wasm_longjmp.
 *
 * These are the three support functions clang's wasm SjLj lowering
 * (-mllvm -wasm-enable-sjlj) leaves undefined in every translation
 * unit that calls setjmp or longjmp; docs/TOOLCHAIN.md first-check
 * (c) records the evidence (the lowered object, its import list and
 * signatures, and the catch glue's reads of the payload). Their
 * contract, as the lowering emits it:
 *
 *   - A setjmp(env) call becomes __wasm_setjmp(env, label, id),
 *     where label is a per-site constant (never 0) and id is the
 *     address of a slot in the caller's frame whose identity marks
 *     this dynamic invocation of the function.
 *   - A longjmp(env, val) call becomes __wasm_longjmp(env, val),
 *     which must not return: it throws the SjLj exception (tag
 *     index 1 in clang's builtin numbering, the C_LONGJMP tag)
 *     carrying the address of the args slot inside env.
 *   - Each setjmp site's catch glue, on catching that exception,
 *     reads the payload's two words (env, value) and calls
 *     __wasm_setjmp_test(env, id) with its own id: nonzero (the
 *     stored label) means the longjmp was aimed at this setjmp,
 *     and the glue resumes it with the value; zero means rethrow
 *     to the next enclosing setjmp site.
 *
 * Written from that contract in this library's own structure; the
 * semantics (label and invocation-id bookkeeping, the args slot
 * riding inside the jmp_buf, the value 0 mapped to 1) are the
 * ones the lowering requires, cross-checked against the reference
 * implementation the vendor toolchain ships in its compiler-rt
 * (read for semantics only, per the program's reference rule).
 *
 * This TU defines the three functions for wasm only. On native
 * targets it is empty: the SjLj names are a wasm lowering artifact
 * and host setjmp is the host C library's.
 */

#include <stddef.h>
#include <stdint.h>

#include "setjmp.h"

#if defined(__wasm__)

/* The jmp_buf's four words, named (see include/setjmp.h). */
typedef struct lk_sjlj_buf {
  void *func_invocation_id;
  uint32_t label;
  void *arg_env;
  int arg_val;
} lk_sjlj_buf_t;

/* clang's builtin tag index for the C longjmp exception. The SjLj
 * lowering's catch clauses catch this same tag, by construction. */
#define LK_SJLJ_TAG_C_LONGJMP 1

void __wasm_setjmp(void *env, uint32_t label, void *func_invocation_id) {
  lk_sjlj_buf_t *jb = (lk_sjlj_buf_t *)env;
  jb->func_invocation_id = func_invocation_id;
  jb->label = label;
}

uint32_t __wasm_setjmp_test(void *env, void *func_invocation_id) {
  lk_sjlj_buf_t *jb = (lk_sjlj_buf_t *)env;
  if (jb->func_invocation_id == func_invocation_id) return jb->label;
  return 0;
}

void __wasm_longjmp(void *env, int val) {
  lk_sjlj_buf_t *jb = (lk_sjlj_buf_t *)env;
  /* The standard's rule: a longjmp value of 0 makes setjmp return
   * 1. Applied here, once, so the payload always carries the
   * value the resumed setjmp will produce. */
  if (val == 0) val = 1;
  jb->arg_env = env;
  jb->arg_val = val;
  __builtin_wasm_throw(LK_SJLJ_TAG_C_LONGJMP, &jb->arg_env);
  __builtin_unreachable();
}

#endif /* __wasm__ */
