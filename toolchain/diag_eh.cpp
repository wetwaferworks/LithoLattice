// Diagnostic driver for the EH probe (toolchain piece use only).
// Runs the EH checks one at a time with a marker print before and
// after each, so a trap names the failing case. Linked INSTEAD of
// probe_main.o against the same library objects.
#include <stdio.h>

extern "C" int lk_eh_cleanup_test();

struct LKProbeBase {
  virtual ~LKProbeBase() {}
  virtual int kind() const { return 1; }
  int base_field;
};
struct LKProbeError {
  int code;
};

// The throw helpers are C++ functions in check_eh_throw.cpp; wrap
// access through these local declarations matching their mangling.
[[noreturn]] void lk_eh_throw_plain(int code);
[[noreturn]] void lk_eh_throw_derived();

static unsigned char g_tls_block[64 * 1024];
extern "C" void *lk_probe_tls_block() { return g_tls_block; }

// The probe startup the libraries expect (same as probe_main.c).
#include "waferheap.h"
#include <string.h>
extern "C" {
int lw_main_thread_init(void);
void __litholibc_errno_arm_tls(void);
}
static unsigned char g_arena[1 << 20];

extern "C" int main() {
  wh_options_t opts;
  memset(&opts, 0, sizeof opts);
  wh_heap_t *heap = wh_heap_create(g_arena, sizeof(g_arena), &opts);
  if (!heap) return 90;
  wh_set_default_heap(heap);
  (void)lw_main_thread_init();
  __litholibc_errno_arm_tls();

  printf("diag: start\n");
  try {
    lk_eh_throw_plain(7);
  } catch (const LKProbeError &e) {
    printf("diag: case1 caught code=%d\n", e.code);
  }
  printf("diag: case1 done\n");
  try {
    lk_eh_throw_derived();
  } catch (const LKProbeBase &b) {
    const int *w = (const int *)(const void *)&b;
    printf("diag: case2 obj=%p words[0..3]=%d %d %d %d kind=%d\n",
           (const void *)&b, w[0], w[1], w[2], w[3], b.kind());
  }
  printf("diag: case2 done\n");
  try {
    try {
      lk_eh_throw_plain(9);
    } catch (const LKProbeError &) {
      printf("diag: case3 inner caught, rethrowing\n");
      throw;
    }
  } catch (const LKProbeError &e) {
    printf("diag: case3 outer caught code=%d\n", e.code);
  }
  printf("diag: case3 done\n");
  try {
    lk_eh_throw_plain(3);
  } catch (...) {
    printf("diag: case4 caught all\n");
  }
  printf("diag: case4 done\n");
  int c = lk_eh_cleanup_test();
  printf("diag: case5 cleanup dtors=%d\n", c);
  printf("diag: all done\n");
  return 0;
}
