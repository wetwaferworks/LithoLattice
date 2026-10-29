/* check_sjlj.c ... toolchain piece, first-check (c): what stock
 * clang --target=wasm32 does with setjmp / longjmp under three
 * flag regimes. No libc header is used on purpose: the probe
 * declares the pair itself so the compiler's treatment of the
 * NAMES is what is measured, not a header's.
 *
 *   A. default flags
 *   B. -mllvm -wasm-enable-sjlj
 *   C. -mllvm -wasm-enable-sjlj -fwasm-exceptions
 *
 * For each: llvm-nm -u on the object lists the runtime support
 * the lowering leaves undefined; that list IS the evidence.
 */
typedef struct {
  unsigned int opaque[32];
} lk_jmp_buf[1];

int setjmp(lk_jmp_buf env);
void longjmp(lk_jmp_buf env, int val);

static lk_jmp_buf lk_env;
static int lk_stage;

__attribute__((noinline)) static void lk_deep(int n) {
  if (n <= 0) {
    longjmp(lk_env, 7);
  }
  lk_deep(n - 1);
  lk_stage = -1; /* must never run: the longjmp skips it */
}

int lk_sjlj_probe(void) {
  lk_stage = 0;
  int v = setjmp(lk_env);
  if (v == 0) {
    lk_deep(3);
    return -100; /* deep returned normally: broken */
  }
  if (v != 7) return -101;
  /* Second arm of the same buffer: longjmp with value 0 must make
   * setjmp return 1 (the standard's rule). Full pass = 71. */
  v = setjmp(lk_env);
  if (v == 0) {
    longjmp(lk_env, 0);
    return -102;
  }
  if (v != 1) return -103;
  return 71;
}
