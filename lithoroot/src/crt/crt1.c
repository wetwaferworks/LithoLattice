/* crt1.c ... LithoRoot's program start-up for the wasm32 module.
 *
 * The host (lkboot) instantiates the module, calls the exported
 * __wasm_call_ctors once, then calls the exported __main_argc_argv
 * (clang's wasm name for int main(int, char**)). There is no _start:
 * the module is linked --no-entry and the host owns the order.
 *
 * What a C runtime must do before any constructor allocates runs
 * here, as the FIRST constructor (priority 0, reserved for the
 * implementation, and LithoRoot is the implementation):
 *
 *   1. the shared heap (lithoheap.h): lock-free per-thread WaferHeap
 *      arenas over linear memory above __heap_base, growing through
 *      lithosys_memory_grow (never the memory.grow instruction; the
 *      host enforces its 600 MiB cap and we see a refusal);
 *   2. the main thread: attached to the weave as tid 1;
 *   3. errno: armed for per-thread storage.
 *
 * No lock anywhere: each thread owns its arena and cross-thread
 * frees ride a lock-free list to the owner (lithothread_heap.c).
 */
#include <stddef.h>
#include <stdint.h>

#include "lithosys.h"
#include "lithoweave.h"
#include "lithoheap.h"

void __litholibc_errno_arm_tls(void);

extern unsigned char __heap_base;

#define LR_PAGE 65536u

/* Commit callback for the shared heap (lithoheap.h): grow linear
 * memory until it covers end. Several threads may grow at once; each
 * re-reads memory.size after its own grow, so the worst case is some
 * over-growth, never a gap. At least 16 MiB per host call so a
 * stream of small chunks does not turn into a stream of host calls. */
static int lr__commit(uintptr_t end, void* user) {
  (void)user;
  for (;;) {
    uintptr_t have = (uintptr_t)__builtin_wasm_memory_size(0) * LR_PAGE;
    uint32_t pages, old_pages = 0;
    if (have >= end) return 1;
    pages = (uint32_t)((end - have + LR_PAGE - 1u) / LR_PAGE);
    if (pages < 256u) pages = 256u;
    if (lithosys_memory_grow(pages, &old_pages) != LITHOSYS_OK) {
      /* a smaller ask may still fit under the cap */
      pages = (uint32_t)((end - have + LR_PAGE - 1u) / LR_PAGE);
      if (lithosys_memory_grow(pages, &old_pages) != LITHOSYS_OK) {
        return (uintptr_t)__builtin_wasm_memory_size(0) * LR_PAGE >= end;
      }
    }
  }
}

/* Start-up status, exported to the host (engine/lithoroot/CMakeLists.txt):
 * 0 ready, 1 heap only (weave attach failed), -1 crt1 never ran. */
int __lithoroot_crt_status = -1;

__attribute__((constructor(0))) static void lr__crt_start(void) {
  uintptr_t base = (uintptr_t)&__heap_base;
  /* The shared lock-free heap: per-thread arenas carved from linear
   * memory above __heap_base (see lithoheap.h). No lock hooks. */
  if (!lh_install(base, (uintptr_t)0xFFFF0000u, lr__commit, NULL)) {
    static const char msg[] = "lithoroot: crt1 could not install the shared heap\n";
    lithosys_console_write(msg, (uint32_t)(sizeof msg - 1u));
    lithosys_abort();
  }
  __lithoroot_crt_status = 1;
  if (lw_main_thread_init() != LITHOSYS_OK) {
    __lithoroot_crt_status = 2;
    return;
  }
  __litholibc_errno_arm_tls();
  __lithoroot_crt_status = 0;
}
