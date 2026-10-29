/* libc_new_delete_test.cpp ... host smoke test for the
 * LithoLibC operator new / delete family
 * (src/libc/new_delete.cc) and its completion of LithoAbi's
 * debt: LithoAbi's own objects reference _ZnwmPv (placement
 * new) and _ZdlPvm (sized delete); this link includes the
 * LithoAbi objects, so the operators below are what satisfy
 * them, exactly as they will in the engine link.
 *
 * Native build and run (this file is a test, not part of the
 * library). The TU is compiled -fno-exceptions, in the house
 * pattern of tests/lithoabi_test.cpp: the throwing paths are
 * exercised through forked children and process outcomes
 * (handler invocation byte, SIGABRT, exit code and atexit
 * order), because host try/catch unwinds through the host EH
 * machinery, which is not the machinery under test.
 *
 * std::set_new_handler / std::get_new_handler are DEFINED by
 * new_delete.cc but not yet DECLARED by <new> (the header is
 * LithoAbi's piece and this piece may not widen it; see
 * docs/LITHOLIBC-STDIO.md). They are declared locally below,
 * with the standard signatures, purely so the smoke can call
 * the definitions it is testing.
 *
 * Build (run from the lithoroot directory):
 *   # C library + platform objects (objects in /tmp/lr/plain
 *   # per tests/libc_stdio_test.c's recipe), then LithoAbi:
 *   REN=""; for n in <the $NAMES list in
 *       tests/lithothread_test.c>; do
 *     REN="$REN -Dpthread_$n=lt_pthread_$n -D$n=lt_$n"; done
 *   # (the exact loop is spelled out in lithoabi_test.cpp)
 *   for f in eh_runtime std_exceptions stubs typeinfo \
 *            personality backend_native_scaffold; do
 *     g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *         -I src/lithoabi -c src/lithoabi/$f.cc -o /tmp/lr/abi/$f.o
 *   done
 *   for f in exception atexit guard; do
 *     g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *         -I src/lithoabi $REN -c src/lithoabi/$f.cc \
 *         -o /tmp/lr/abi/$f.o
 *   done
 *   g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *       -c src/libc/new_delete.cc -o /tmp/lr/abi/new_delete.o
 *   # One bench adjustment (see below): a copy of the native
 *   # back-end object with its host exit/abort references
 *   # renamed, replacing the plain-build copy:
 *   objcopy --redefine-sym exit=lk_host_exit \
 *           --redefine-sym abort=lk_host_abort \
 *           /tmp/lr/plain/lithoweave_native.o \
 *           /tmp/lr/smoke/lithoweave_native.o
 *   g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *       tests/libc_new_delete_test.cpp (the abi objects) \
 *       (the smoke objects) \
 *       -o /tmp/libc_new_delete_test -nostdlib++ -ldl -pthread
 *   /tmp/libc_new_delete_test
 *
 * Why the objcopy step: this binary links LithoLibC's own
 * exit() and abort() (stdlib.c, under test here) AND the
 * native back-end, whose lithosys_exit / lithosys_abort are
 * implemented BY CALLING the host exit / abort. Symbol
 * interposition sends those calls back into LithoLibC's
 * versions, which call lithosys_exit / lithosys_abort again:
 * a loop that ends in stack exhaustion (observed as SIGSEGV
 * after otherwise-correct behavior). Renaming the back-end's
 * two references makes them bind to the host libc directly.
 * The loop is a native-bench artifact only: on wasm,
 * lithosys_exit and lithosys_abort are host imports, not calls
 * back into libc. (An earlier attempt used -Wl,--wrap=abort;
 * __real_abort still resolved to the executable's own abort,
 * so the loop survived. Renaming the references is the fix
 * that actually breaks it.)
 */

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <new>
#include <stdlib.h> /* LithoLibC (via -I include) */
#include "waferheap.h"

/* The definitions all exist in new_delete.cc; the declarations
 * have not reached <new> yet (see the header note). The smoke
 * declares the standard forms locally so the language syntax
 * below binds to the definitions under test. */
namespace std {
new_handler set_new_handler(new_handler) noexcept;
new_handler get_new_handler() noexcept;
} /* namespace std */

void* operator new(std::size_t);
void* operator new[](std::size_t);
void* operator new(std::size_t, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t, const std::nothrow_t&) noexcept;
void* operator new(std::size_t, void*) noexcept;
void* operator new[](std::size_t, void*) noexcept;
void* operator new(std::size_t, std::align_val_t);
void* operator new[](std::size_t, std::align_val_t);
void operator delete(void*) noexcept;
void operator delete[](void*) noexcept;
void operator delete(void*, std::size_t) noexcept;
void operator delete[](void*, std::size_t) noexcept;
void operator delete(void*, std::size_t, std::align_val_t) noexcept;
void operator delete[](void*, std::size_t, std::align_val_t) noexcept;

/* ---- verdict plumbing: host vsnprintf + write(2), so the
 * code under test never grades itself ---- */
typedef int (*vsnprintf_t)(char*, size_t, const char*, va_list);
static vsnprintf_t h_vsnprintf;
static void report(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = h_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    ssize_t w = write(1, buf, (size_t)n);
    (void)w;
  }
}

static int g_failures = 0;
#define CASE_BEGIN(name) \
  const char* case_name = (name); \
  int case_ok = 1
#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      case_ok = 0;                                        \
      report("  check failed: %s (line %d)\n", #cond,     \
             __LINE__);                                   \
    }                                                     \
  } while (0)
#define CASE_END()                                      \
  do {                                                  \
    report("%s: %s\n", case_ok ? "PASS" : "FAIL",       \
           case_name);                                  \
    if (!case_ok) ++g_failures;                         \
  } while (0)

/* ---- WaferHeap default heap over a static arena ---- */
static unsigned char g_arena[32 * 1024 * 1024];
static void* grow_refused(wh_heap_t*, size_t, size_t*, void*) {
  return NULL;
}
static void setup_heap(void) {
  wh_options_t opts;
  memset(&opts, 0, sizeof opts);
  opts.grow = grow_refused;
  wh_heap_t* heap = wh_heap_create(g_arena, sizeof g_arena, &opts);
  if (heap != NULL) wh_set_default_heap(heap);
}

/* ---- fixtures ---- */
static int g_ctors = 0;
static int g_dtors = 0;
struct Widget {
  int v;
  Widget() : v(7) { ++g_ctors; }
  ~Widget() { ++g_dtors; }
};
struct alignas(64) OverAligned {
  double x[4];
};

/* child A: the new-handler must be called on failure */
static int g_pipe_w = -1;
static void handler_marks_and_exits(void) {
  char c = 'H';
  ssize_t w = write(g_pipe_w, &c, 1);
  (void)w;
  _exit(42);
}

/* child C: atexit ordering marks */
static void mark(char c) {
  ssize_t w = write(g_pipe_w, &c, 1);
  (void)w;
}
static void h1(void) { mark('1'); }
static void h2(void) { mark('2'); }
static void dmark(void*) { mark('D'); }

extern "C" int __cxa_atexit(void (*f)(void*), void* arg, void* dso);
extern "C" void* __dso_handle;

/* Aliases for the native back-end's renamed host references
 * (see the build note): reach glibc's own exit / abort through
 * dlsym so interposition cannot loop them back into the libc
 * under test. */
extern "C" void lk_host_exit(int code) {
  typedef void (*exit_t)(int);
  static exit_t real_exit = NULL;
  if (real_exit == NULL) {
    void* libc = dlopen("libc.so.6", RTLD_NOW);
    real_exit = (exit_t)dlsym(libc, "exit");
  }
  real_exit(code);
  _exit(127);
}
extern "C" void lk_host_abort(void) {
  raise(SIGABRT);
  _exit(134);
}

int main(void) {
  void* libc = dlopen("libc.so.6", RTLD_NOW);
  h_vsnprintf = (vsnprintf_t)dlsym(libc, "vsnprintf");
  if (h_vsnprintf == NULL) return 2;
  setup_heap();

  {
    CASE_BEGIN("scalar / array new-delete, ctor and dtor counts");
    int* p = new int(42);
    CHECK(p != NULL && *p == 42);
    delete p;
    g_ctors = g_dtors = 0;
    Widget* w = new Widget;
    CHECK(w != NULL && w->v == 7 && g_ctors == 1);
    delete w;
    CHECK(g_dtors == 1);
    Widget* arr = new Widget[4];
    CHECK(arr != NULL && g_ctors == 5);
    delete[] arr;
    CHECK(g_dtors == 5);
    double* d = new double[16];
    CHECK(d != NULL);
    d[15] = 2.5;
    CHECK(d[15] == 2.5);
    delete[] d;
    CASE_END();
  }

  {
    CASE_BEGIN("placement new returns its address (_ZnwmPv)");
    alignas(Widget) unsigned char buf[sizeof(Widget)];
    g_ctors = g_dtors = 0;
    Widget* w = new (buf) Widget;
    CHECK((void*)w == (void*)buf);
    CHECK(w->v == 7 && g_ctors == 1);
    w->~Widget();
    CHECK(g_dtors == 1);
    /* also through the raw operator symbol form */
    void* q = operator new(sizeof(Widget), (void*)buf);
    CHECK(q == (void*)buf);
    CASE_END();
  }

  {
    CASE_BEGIN("sized delete (_ZdlPvm) and unsized, null safety");
    void* p = operator new(128);
    CHECK(p != NULL);
    operator delete(p, 128);
    void* q = operator new(64);
    operator delete(q);
    operator delete(NULL);
    operator delete(NULL, 64);
    CASE_END();
  }

  {
    CASE_BEGIN("aligned new / delete (align_val_t)");
    void* p = operator new(64, std::align_val_t(64));
    CHECK(p != NULL && ((uintptr_t)p % 64) == 0);
    operator delete(p, 64, std::align_val_t(64));
    OverAligned* oa = new OverAligned;
    CHECK(oa != NULL && ((uintptr_t)oa % 64) == 0);
    delete oa;
    CASE_END();
  }

  {
    CASE_BEGIN("nothrow new fails soft; handler accessors");
    void* p = operator new(SIZE_MAX / 2, std::nothrow);
    CHECK(p == NULL);
    void* q = operator new[](SIZE_MAX / 2, std::nothrow);
    CHECK(q == NULL);
    std::new_handler prev = std::set_new_handler(handler_marks_and_exits);
    CHECK(std::get_new_handler() == handler_marks_and_exits);
    std::new_handler back = std::set_new_handler(prev);
    CHECK(back == handler_marks_and_exits);
    CHECK(std::get_new_handler() == prev);
    CASE_END();
  }

  {
    CASE_BEGIN("child: new-handler runs on throwing-new failure");
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]);
      g_pipe_w = fds[1];
      std::set_new_handler(handler_marks_and_exits);
      void* p = operator new(SIZE_MAX / 2); /* must not return */
      (void)p;
      _exit(99); /* handler never ran */
    }
    close(fds[1]);
    char c = 0;
    ssize_t n = read(fds[0], &c, 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(n == 1 && c == 'H');
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    CASE_END();
  }

  {
    CASE_BEGIN("child: throwing new with no handler aborts "
               "(bad_alloc through LithoAbi)");
    pid_t pid = fork();
    if (pid == 0) {
      void* p = operator new(SIZE_MAX / 2);
      (void)p;
      _exit(99); /* returned: wrong */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    report("  child status: exited=%d code=%d signaled=%d sig=%d\n",
           WIFEXITED(status) ? 1 : 0,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? 1 : 0,
           WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CASE_END();
  }

  {
    CASE_BEGIN("child: exit() runs the atexit registry LIFO, "
               "then lithosys_exit(code)");
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]);
      g_pipe_w = fds[1];
      /* Registration order: dtor, h1, h2. LIFO run: 2, 1, D. */
      __cxa_atexit(dmark, NULL, &__dso_handle);
      atexit(h1);
      atexit(h2);
      exit(7); /* LithoLibC exit: __cxa_finalize, flush, out */
      _exit(99);
    }
    close(fds[1]);
    char buf[8];
    /* Read to EOF: a single read returns after the first mark,
     * and closing early SIGPIPEs the child mid-registry. */
    ssize_t n = 0;
    for (;;) {
      ssize_t r = read(fds[0], buf + n, sizeof buf - (size_t)n);
      if (r <= 0) break;
      n += r;
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    report("  child status: exited=%d code=%d signaled=%d sig=%d, "
           "marks=%ld\n",
           WIFEXITED(status) ? 1 : 0,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? 1 : 0,
           WIFSIGNALED(status) ? WTERMSIG(status) : -1, (long)n);
    CHECK(n == 3 && buf[0] == '2' && buf[1] == '1' && buf[2] == 'D');
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CASE_END();
  }

  report(g_failures == 0 ? "ALL GROUPS PASS\n" : "%d GROUP(S) FAILED\n",
         g_failures);
  return g_failures == 0 ? 0 : 1;
}
