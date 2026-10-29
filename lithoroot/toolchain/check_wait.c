/* check_wait.c ... toolchain piece, first-check (a): the exact
 * wait / notify builtin spellings LithoThread and LithoWeave use,
 * isolated so a spelling failure is a one-file event.
 *
 * The call shapes below are copied verbatim from the two call
 * sites in the gated pieces:
 *   src/lithothread.c (lt__wait32 / lt__notify, wasm provider)
 *   src/lithoweave.c  (lw__cell_wait, wasm provider)
 * Compile: clang --target=wasm32 -matomics -mbulk-memory -O1 -c
 */
#include <stdint.h>

static volatile uint32_t lk_check_word;

int lk_check_wait32(int64_t timeout_ns) {
  /* lithothread.c:1307 and lithoweave.c:513, verbatim shape. */
  return (int)__builtin_wasm_memory_atomic_wait32(
      (int *)(void *)&lk_check_word, (int)0, timeout_ns);
}

uint32_t lk_check_notify(int32_t count) {
  /* lithothread.c:1312, verbatim shape. */
  return (uint32_t)__builtin_wasm_memory_atomic_notify(
      (int *)(void *)&lk_check_word, (int)count);
}
