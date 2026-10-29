/* Diagnostic: what does our snprintf actually produce under wasm? */
#include <stdio.h>
#include <string.h>
#include "waferheap.h"
#include "lithoweave.h"

static unsigned char lk_arena[1 << 20];
static unsigned char lk_tls_storage[65536] __attribute__((aligned(16)));
void *lk_probe_tls_block(void) { return lk_tls_storage; }
void __litholibc_errno_arm_tls(void);

int main(void) {
  wh_options_t opts;
  memset(&opts, 0, sizeof opts);
  wh_heap_t *heap = wh_heap_create(lk_arena, sizeof lk_arena, &opts);
  if (!heap) return 90;
  wh_set_default_heap(heap);
  (void)lw_main_thread_init();
  __litholibc_errno_arm_tls();

  char buf[64];
  int n = snprintf(buf, sizeof buf, "%d-%s-%u", 42, "litho", 7u);
  printf("diag: n=%d buf=[%s]\n", n, buf);
  char b2[64];
  int n2 = snprintf(b2, sizeof b2, "%d %d %d", 11, 22, 33);
  printf("diag: n2=%d b2=[%s]\n", n2, b2);
  return 0;
}
