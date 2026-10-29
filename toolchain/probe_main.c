/* probe_main.c ... toolchain piece, the headline probe's main.
 *
 * Linked with every wasm32 LithoRoot object (probe link, see
 * docs/TOOLCHAIN.md for the exact commands) and run under the
 * stub host in run_probe.mjs. It exercises, at run time and not
 * just link time:
 *   - WaferHeap as the default heap over a fixed arena
 *   - LithoLibC malloc / snprintf / strcmp
 *   - LithoThread pthread_create: the stub host refuses the spawn
 *     with -LITHOSYS_EAGAIN, so the create must fail SOFT with an
 *     ordinary error code, never trap (the freeze's contract)
 *   - LithoAbi C++ throw / catch across TU boundaries, through
 *     lk_eh_probe() in check_eh_catch.cpp (expects mask 31)
 * Return value: 0 when every check passes, else a failure bitmask
 * (bit per check, in source order). printf at the end also drives
 * the frozen lithosys_fd_write import on fd 1.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "lithosys.h"
#include "lithoweave.h"
#include "waferheap.h"

/* Startup entry from src/lithothread_errno.c; arming is startup's
 * job in the LithoThread link rule, and the probe is the startup. */
void __litholibc_errno_arm_tls(void);

/* check_eh_catch.cpp. */
int lk_eh_probe(void);

static unsigned char lk_arena[1 << 20];

/* TLS storage for the main thread. In a shared-memory link the
 * host owes each thread a TLS block through the exported
 * __wasm_init_tls before any thread-local is touched (the weave
 * and thread pieces keep their current-thread pointers in TLS).
 * The real host does this per worker in the thread-start
 * protocol (see the lkboot patch plan in docs/TOOLCHAIN.md); the
 * probe's Node host calls __wasm_init_tls on this static block
 * for the main thread. Plain static storage, no TLS, no malloc,
 * so the helper itself is safe to call first. */
static unsigned char lk_tls_storage[65536] __attribute__((aligned(16)));
void *lk_probe_tls_block(void) { return lk_tls_storage; }

static void *lk_worker(void *arg) {
  return (void *)(uintptr_t)((uintptr_t)arg + 1u);
}

int main(void) {
  int failures = 0;
  wh_options_t opts;
  wh_heap_t *heap;

  memset(&opts, 0, sizeof opts); /* no grow callback: fixed arena */
  heap = wh_heap_create(lk_arena, sizeof lk_arena, &opts);
  if (heap == NULL) return 90;
  wh_set_default_heap(heap);

  if (lw_main_thread_init() != LITHOSYS_OK) failures |= 1;
  __litholibc_errno_arm_tls();

  /* malloc round trip through WaferHeap. */
  {
    char *p = (char *)malloc(64);
    if (p == NULL) {
      failures |= 2;
    } else {
      memset(p, 0xAB, 64);
      if ((unsigned char)p[63] != 0xAB) failures |= 2;
      free(p);
    }
  }

  /* snprintf through our printf family. */
  {
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%d-%s-%u", 42, "litho", 7u);
    if (n != 10 || strcmp(buf, "42-litho-7") != 0) failures |= 4;
  }

  /* pthread_create against a refusing host: fail soft or succeed
   * honestly, nothing in between. */
  {
    pthread_t th;
    int rc = pthread_create(&th, NULL, lk_worker, (void *)(uintptr_t)41u);
    if (rc == 0) {
      void *ret = NULL;
      if (pthread_join(th, &ret) != 0 || ret != (void *)(uintptr_t)42u)
        failures |= 8;
    } else if (rc != EAGAIN) {
      failures |= 8;
    }
  }

  /* C++ exceptions across TU boundaries, via LithoAbi. */
  if (lk_eh_probe() != 31) failures |= 16;

  printf("lk-probe: failures=%d\n", failures);
  return failures;
}
