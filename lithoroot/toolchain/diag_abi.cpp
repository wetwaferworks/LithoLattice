// Diagnostic: manual __cxa throw with a known destructor.
// Bisects compiler-emitted throw sequences vs LithoAbi bookkeeping.
#include <stdio.h>
#include <string.h>
#include <typeinfo>
#include "waferheap.h"

inline void *operator new(unsigned long, void *p) noexcept { return p; }

extern "C" {
void *__cxa_allocate_exception(unsigned long);
/* The wasm destructor function type: on this target a destructor
 * returns its argument (see docs/TOOLCHAIN.md, first-check (b)). */
void __cxa_throw(void *, std::type_info *, void *(*)(void *))
    __attribute__((__noreturn__));
}

struct DiagError {
  int code;
};
// typeinfo for DiagError: emitted by this TU because the catch
// below names the type.
static int g_dtor_calls = 0;
static void *diag_dtor(void *p) {
  DiagError *e = (DiagError *)p;
  printf("diagabi: dtor called, code=%d\n", e->code);
  ++g_dtor_calls;
  return p;
}

static unsigned char g_tls_block[64 * 1024];
extern "C" void *lk_probe_tls_block() { return g_tls_block; }

extern "C" {
int lw_main_thread_init(void);
void __litholibc_errno_arm_tls(void);
}
static unsigned char g_arena[1 << 20];
static void *(*volatile g_sink)(void *) = 0;

int main() {
  wh_options_t opts;
  memset(&opts, 0, sizeof opts);
  wh_heap_t *heap = wh_heap_create(g_arena, sizeof(g_arena), &opts);
  if (!heap) return 90;
  wh_set_default_heap(heap);
  (void)lw_main_thread_init();
  __litholibc_errno_arm_tls();
  g_sink = (void *(*)(void *))&diag_dtor; // force address-taken

  printf("diagabi: varargs check %d %d %d\n", 11, 22, 33);
  try {
    void *mem = __cxa_allocate_exception(sizeof(DiagError));
    DiagError *e = new (mem) DiagError{42};
    __cxa_throw(e, (std::type_info *)&typeid(DiagError), &diag_dtor);
  } catch (const DiagError &e) {
    printf("diagabi: caught code=%d\n", e.code);
  }
  printf("diagabi: after catch, dtor_calls=%d\n", g_dtor_calls);
  return 0;
}
