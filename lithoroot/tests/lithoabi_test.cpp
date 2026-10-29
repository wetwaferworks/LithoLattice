/* lithoabi_test.cpp ... host functional test for LithoAbi.
 *
 * Native build and run (this file is a test, not part of the
 * library). The library objects are compiled with OUR headers
 * first (-I include) and -fno-exceptions: LithoAbi is freestanding
 * and must not grow host EH dependencies. The LithoWeave and
 * LithoThread native backends are compiled with HOST headers only
 * (they play the host). The test itself is compiled with
 * -fno-exceptions too and drives the __cxa_* entry points by hand:
 * no try / catch / throw keywords appear anywhere below, because
 * the host compiler's own EH machinery must not be what gets
 * exercised. Landing pads are emulated with the scaffolding
 * backend (src/lithoabi/lithoabi_test_scaffold.h): a pushed frame,
 * a setjmp landing, __do_catch matching by hand, then
 * __cxa_begin_catch / __cxa_end_catch, exactly the sequence
 * compiler glue performs.
 *
 * The link uses -nostdlib++: the only C++ runtime in the binary is
 * LithoAbi itself, so every __cxa_* call, every dynamic_cast, and
 * every typeid comparison in this file resolves to the code under
 * test.
 *
 * Plain (run from the lithoroot directory; objects land in /tmp):
 *   gcc -std=c99 -pedantic -Wall -Wextra -I include \
 *       -c src/waferheap.c -o /tmp/wh.o        (plus the other
 *       plain C pieces: lithoweave.c and the LithoLibC objects
 *       src/libc/memory.c string.c strings.c ctype.c malloc.c
 *       strerror.c, same flags. src/lithothread_errno.c replaces
 *       src/libc/errno.c in any link that includes LithoThread:
 *       both provide __litholibc_errno_location, LithoThread's
 *       being the thread-aware one, so errno.c is not linked
 *       here.)
 *   The LithoThread objects (lithothread.c, lithothread_errno.c,
 *   lithothread_heap.c) are compiled with the house rename
 *   discipline from tests/lithothread_test.c: every pthread_*
 *   function name is compiled as lt_pthread_* (see that file for
 *   the NAMES list and the $REN loop), so LithoThread does not
 *   interpose the host pthread symbols the native backends call.
 *   LithoAbi's own pthread consumers (exception.cc, atexit.cc,
 *   guard.cc) are compiled with the same $REN defines, so their
 *   pthread calls land on LithoThread's implementations, matching
 *   the layouts in include/pthread.h.
 *   gcc -std=c99 -Wall -Wextra -c src/lithoweave_native.c -o /tmp/lwn.o
 *   gcc -std=c99 -Wall -Wextra -c src/lithothread_native.c -o /tmp/ltn.o
 *   for f in eh_runtime std_exceptions stubs typeinfo personality \
 *            backend_native_scaffold; do
 *     g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *         -I src/lithoabi -c src/lithoabi/$f.cc -o /tmp/$f.o
 *   done
 *   for f in exception atexit guard; do
 *     g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *         -I src/lithoabi $REN -c src/lithoabi/$f.cc -o /tmp/$f.o
 *   done
 *   g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include \
 *       -I src/lithoabi tests/lithoabi_test.cpp \
 *       (the objects above, from /tmp) \
 *       -o /tmp/lithoabi_test -nostdlib++ -pthread
 *   /tmp/lithoabi_test
 *
 * ASan + UBSan: add -g -fsanitize=address,undefined to every
 * compile and the link, EXCEPT src/libc/malloc.c and
 * src/lithothread_errno.c, which are compiled plain (the house
 * rule: an instrumented interposed malloc faults inside the
 * sanitizer's own initialization; see tests/lithoweave_test.c).
 * The C++ translation units additionally take -fno-sanitize=vptr:
 * UBSan's vptr check recurses through its dynamic-type cache-miss
 * handler when the code under test IS the RTTI implementation
 * (observed as a stack overflow inside the handler); the piece's
 * own cases verify the RTTI behavior directly.
 *
 * TSan: as the plain build but add -g -fsanitize=thread to every
 * compile and -no-pie to the link, EXCEPT the whole malloc path
 * (src/libc/malloc.c, src/waferheap.c, src/lithothread_errno.c)
 * compiled plain, and run under setarch -R with
 * TSAN_OPTIONS=halt_on_error=0 (the arrangement the LithoThread
 * test documented for this host).
 *
 * What this does NOT prove is listed in docs/LITHOABI.md; the short
 * version: the wasm throw itself (tag, intrinsic spellings, the
 * compiler's landing-pad glue) awaits the toolchain piece. What is
 * proven here is everything portable: allocation and header
 * discipline, refcount lifetime, catch matching and adjustment,
 * the personality decision procedure over real LSDA bytes, guards,
 * atexit wiring, dynamic_cast, and the terminate paths, all
 * against the scaffolding's faithful delivery model.
 *
 * Death cases run by re-executing this binary with --die N in a
 * child process; the parent asserts the child died of SIGABRT,
 * which is the native shape of lithosys_abort.
 */

/* Audit fix D-F5 (fix run, 2026-10-28): since the stdio piece
 * landed, this tree's include/stdio.h shadows the host header for
 * this translation unit, and the host <cstdio> below requires
 * ::freopen, ::tmpfile and ::tmpnam, which LithoLibC deliberately
 * does not declare. The three host declarations are restated here
 * with their host signatures so the host header's
 * using-declarations resolve. Test-file-only change: the
 * production header surface is exactly as the stdio piece left
 * it, and nothing in this test calls any of the three. */
#include <stdio.h> /* LithoLibC (via -I include): FILE, for the declarations below */
extern "C" {
FILE* freopen(const char* filename, const char* mode, FILE* stream);
FILE* tmpfile(void);
char* tmpnam(char* s);
}

/* The same shadowing reaches <time.h>: this TU mixes host headers
 * (<ctime> for the test's own timing, <csignal> for the death
 * cases) with the tree's headers, and LithoLibC's time.h typedefs
 * time_t and clock_t in ways that conflict with the host's, while
 * not declaring timespec_get or nanosleep, both of which this
 * test uses from the host. The host time header is therefore
 * included by its real path and the LithoLibC header's guard is
 * pre-defined so its own <time.h> lookup in this TU comes up
 * empty, restoring exactly the arrangement this test built under
 * before the time header existed. No LithoAbi header includes
 * <time.h>, so nothing under test loses a declaration. */
#define LITHOLIBC_TIME_H
#include </usr/include/time.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <ctime>
#include <stdlib.h> /* LithoLibC (via -I include): malloc / free */
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "lithosys.h"
#include "lithoweave.h"
#include "waferheap.h"
#include "cxxabi.h"
#include "lithoabi_internal.h"
#include "lithoabi_atomic.h"
#include "lithoabi_test_scaffold.h"

namespace abi = __cxxabiv1;

/* Placement new for the test's own constructions in exception
 * storage, and the sized delete the test classes' deleting
 * destructors reference. Both are defined here as test
 * scaffolding: the allocating operator new / delete family is the
 * stdlib piece's surface, not LithoAbi's, and nothing in this test
 * ever calls the delete (all objects live on the stack or in
 * exception storage released by the runtime); if it ever is
 * called, that is a test bug and it ends loudly. */
void* operator new(std::size_t, void* p) noexcept { return p; }
void operator delete(void*) noexcept { lithosys_abort(); }
void operator delete(void*, std::size_t) noexcept { lithosys_abort(); }

/* The LithoSys console sink, defined by the test: diagnostics go
 * to stderr, unbuffered, so death-case output survives the abort. */
extern "C" void lithosys_console_write(const char* buf, uint32_t len) {
  (void)!std::fwrite(buf, 1, len, stderr);
  std::fflush(stderr);
}

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL check: %s (line %d)\n", #cond, __LINE__);  \
      ++fails;                                                       \
    }                                                                \
  } while (0)

#define RUN_CASE(fn)                                    \
  do {                                                  \
    int f = fn();                                       \
    std::printf("%s: %s\n", f == 0 ? "PASS" : "FAIL", #fn); \
    g_failures += f;                                    \
  } while (0)

static int g_failures = 0;
static wh_heap_t* g_heap = nullptr;

/* Heap lock hooks: an atomic spinlock (this TU deliberately never
 * includes a pthread header; the library's own TUs carry the real
 * LithoThread locking). */
static volatile int g_heap_spin = 0;
static void heap_lock(void*) {
  while (__atomic_exchange_n(&g_heap_spin, 1, __ATOMIC_ACQUIRE)) {
  }
}
static void heap_unlock(void*) {
  __atomic_store_n(&g_heap_spin, 0, __ATOMIC_RELEASE);
}

/* ---- test exception types --------------------------------------
 * The destructor counters prove exactly-once destruction. Puppy
 * derives from Dog through a second base so the Dog sub-object
 * sits at a nonzero offset and catch adjustment is observable. */

static int g_animal_dtor = 0;
static int g_other_dtor = 0;
static int g_dog_dtor = 0;
static int g_puppy_dtor = 0;
static int g_cat_dtor = 0;

struct Animal {
  virtual ~Animal();
  int a = 0;
};
Animal::~Animal() { __atomic_add_fetch(&g_animal_dtor, 1, __ATOMIC_SEQ_CST); }

struct OtherBase {
  virtual ~OtherBase();
  int o = 0;
};
OtherBase::~OtherBase() {
  __atomic_add_fetch(&g_other_dtor, 1, __ATOMIC_SEQ_CST);
}

struct Dog : Animal {
  ~Dog() override;
  int d = 0;
};
Dog::~Dog() { __atomic_add_fetch(&g_dog_dtor, 1, __ATOMIC_SEQ_CST); }

struct Puppy : OtherBase, Dog {
  ~Puppy() override;
  int p = 0;
};
Puppy::~Puppy() { __atomic_add_fetch(&g_puppy_dtor, 1, __ATOMIC_SEQ_CST); }

struct Cat {
  virtual ~Cat();
  int c = 0;
};
Cat::~Cat() { __atomic_add_fetch(&g_cat_dtor, 1, __ATOMIC_SEQ_CST); }

/* ---- scaffolding helpers --------------------------------------- */

/* Allocate and construct an exception object of type E. */
template <typename E>
E* make_exception() {
  void* mem = abi::__cxa_allocate_exception(sizeof(E));
  return new (mem) E();
}

template <typename E>
void destroy_exception_cb(void* p) {
  static_cast<E*>(p)->~E();
}

/* Throw a constructed object through the real __cxa_throw. */
template <typename E>
[[noreturn]] void throw_exception(E* obj) {
  abi::__cxa_throw(obj, const_cast<std::type_info*>(&typeid(E)),
                   &destroy_exception_cb<E>);
}

/* Run body() inside one scaffold catch frame. Returns true when an
 * exception landed; the frame has been consumed and the header is
 * in ts.header. Returns false on normal completion. */
struct try_scope {
  abi::lithoabi_scaffold_frame frame;
  abi::lithoabi_eh_header* header = nullptr;
};

template <typename F>
bool run_catching(try_scope& ts, F body) {
  abi::lithoabi_scaffold_push_frame(&ts.frame);
  if (LITHOABI_SCAFFOLD_TRY(&ts.frame)) {
    body();
    abi::lithoabi_scaffold_pop_frame();
    ts.header = nullptr;
    return false;
  }
  ts.header = abi::lithoabi_scaffold_take_caught();
  return true;
}

/* The landing pad's matching job, mirroring personality.cc: try
 * the candidate catch type's __do_catch on a scratch pointer; on a
 * match, store the adjusted pointer and enter the catch. A null
 * catch type is catch (...). Returns the adjusted object pointer,
 * or nullptr when the type does not match (no state touched). */
static void* match_and_begin(abi::lithoabi_eh_header* h,
                             const std::type_info* catch_type) {
  void* obj = abi::lithoabi_object_of_header(h);
  if (catch_type != nullptr) {
    void* scratch = obj;
    if (!catch_type->__do_catch(h->exceptionType, &scratch, 0)) {
      return nullptr;
    }
    h->adjustedPtr = scratch;
  } else {
    h->adjustedPtr = obj;
  }
  return abi::__cxa_begin_catch(&h->unwindHeader);
}

static int uncaught_now() {
  abi::__cxa_eh_globals* g = abi::__cxa_get_globals_fast();
  return g ? static_cast<int>(g->uncaughtExceptions) : -1;
}

/* ================================================================
 * Case A: allocation, header layout, free.
 */
static int case_alloc_free_header() {
  int fails = 0;
  void* obj = abi::__cxa_allocate_exception(48);
  CHECK(obj != nullptr);
  abi::lithoabi_eh_header* h = abi::lithoabi_header_of_object(obj);
  CHECK(h->referenceCount == 1);
  CHECK(h->unwindHeader.exception_class ==
        abi::lithoabi_cpp_exception_class());
  CHECK((reinterpret_cast<uintptr_t>(obj) & 15u) == 0);
  CHECK(abi::lithoabi_object_of_header(h) == obj);
  CHECK((h->flags & abi::LITHOABI_EH_FLAG_FROM_POOL) == 0);
  abi::__cxa_free_exception(obj);

  /* Larger than the emergency payload: still served, still the
   * heap path under a healthy heap. */
  void* big = abi::__cxa_allocate_exception(1024);
  CHECK(big != nullptr);
  abi::lithoabi_eh_header* bh = abi::lithoabi_header_of_object(big);
  CHECK((bh->flags & abi::LITHOABI_EH_FLAG_FROM_POOL) == 0);
  abi::__cxa_free_exception(big);
  return fails;
}

/* ================================================================
 * Case E1: throw Dog, catch Dog exactly.
 */
static int case_eh_exact_catch() {
  int fails = 0;
  const int dog0 = g_dog_dtor, animal0 = g_animal_dtor;
  try_scope ts;
  Dog* thrown = nullptr;
  bool landed = run_catching(ts, [&] {
    thrown = make_exception<Dog>();
    throw_exception(thrown);
  });
  CHECK(landed);
  CHECK(ts.header != nullptr);
  CHECK(uncaught_now() == 1); /* charged, not yet caught */
  CHECK(ts.header->exceptionType == &typeid(Dog));
  void* adj = match_and_begin(ts.header, &typeid(Dog));
  CHECK(adj == static_cast<void*>(thrown));
  CHECK(uncaught_now() == 0);
  CHECK(abi::__cxa_current_exception_type() == &typeid(Dog));
  CHECK(abi::__cxa_get_exception_ptr(&ts.header->unwindHeader) == adj);
  CHECK(ts.header->handlerCount == 1);
  abi::__cxa_end_catch();
  CHECK(g_dog_dtor == dog0 + 1);
  CHECK(g_animal_dtor == animal0 + 1);
  CHECK(uncaught_now() == 0);
  CHECK(abi::__cxa_get_globals_fast()->caughtExceptions == nullptr);
  return fails;
}

/* ================================================================
 * Case E2: throw Puppy, catch the second base (Dog) with a real
 * nonzero adjustment; also verify a non-matching type does not
 * disturb the in-flight state.
 */
static int case_eh_base_catch_adjust() {
  int fails = 0;
  const int puppy0 = g_puppy_dtor, dog0 = g_dog_dtor;
  try_scope ts;
  Puppy* thrown = nullptr;
  bool landed = run_catching(ts, [&] {
    thrown = make_exception<Puppy>();
    throw_exception(thrown);
  });
  CHECK(landed);
  /* Cat does not match: nothing may change. */
  CHECK(match_and_begin(ts.header, &typeid(Cat)) == nullptr);
  CHECK(ts.header->handlerCount == 0);
  CHECK(uncaught_now() == 1);
  /* Dog matches through the upcast, adjusted to the Dog base. */
  void* adj = match_and_begin(ts.header, &typeid(Dog));
  Dog* expected = static_cast<Dog*>(thrown);
  CHECK(adj == static_cast<void*>(expected));
  CHECK(adj != static_cast<void*>(thrown)); /* nonzero base offset */
  abi::__cxa_end_catch();
  CHECK(g_puppy_dtor == puppy0 + 1);
  CHECK(g_dog_dtor == dog0 + 1);
  return fails;
}

/* ================================================================
 * Case E3: catch (...) and sequential throws in one frame.
 */
static int case_eh_catch_all() {
  int fails = 0;
  const int cat0 = g_cat_dtor;
  for (int round = 0; round < 3; ++round) {
    try_scope ts;
    Cat* thrown = nullptr;
    bool landed = run_catching(ts, [&] {
      thrown = make_exception<Cat>();
      throw_exception(thrown);
    });
    CHECK(landed);
    void* adj = match_and_begin(ts.header, nullptr);
    CHECK(adj == static_cast<void*>(thrown));
    abi::__cxa_end_catch();
  }
  CHECK(g_cat_dtor == cat0 + 3);
  CHECK(abi::lithoabi_scaffold_throw_count() >= 3);
  CHECK(uncaught_now() == 0);
  return fails;
}

/* ================================================================
 * Case E4: rethrow from an inner handler to an outer frame; the
 * object is the same and dies exactly once, at the outer end_catch.
 */
static int case_eh_rethrow() {
  int fails = 0;
  const int dog0 = g_dog_dtor;
  try_scope outer;
  Dog* thrown = nullptr;
  bool landed_outer = run_catching(outer, [&] {
    try_scope inner;
    bool landed_inner = run_catching(inner, [&] {
      thrown = make_exception<Dog>();
      throw_exception(thrown);
    });
    CHECK(landed_inner);
    void* adj = match_and_begin(inner.header, &typeid(Dog));
    CHECK(adj == static_cast<void*>(thrown));
    /* Handler body decides to rethrow: control leaves via the
     * backend to the outer frame; nothing after this runs here. */
    abi::__cxa_rethrow();
  });
  CHECK(landed_outer);
  CHECK(outer.header != nullptr);
  CHECK(outer.header == abi::lithoabi_header_of_object(thrown));
  CHECK(uncaught_now() == 1);
  void* adj = match_and_begin(outer.header, &typeid(Dog));
  CHECK(adj == static_cast<void*>(thrown));
  CHECK(g_dog_dtor == dog0); /* still alive in the outer handler */
  abi::__cxa_end_catch();
  CHECK(g_dog_dtor == dog0 + 1);
  return fails;
}

/* ================================================================
 * Case E5: the runtime's own typed throws: bad_alloc from the
 * reserved-slot thrower (on a healthy heap here) caught as
 * std::exception, and __cxa_bad_cast caught exactly.
 */
static int case_eh_own_throws() {
  int fails = 0;
  {
    try_scope ts;
    bool landed = run_catching(ts, [] { abi::lithoabi_throw_bad_alloc(); });
    CHECK(landed);
    CHECK(ts.header->exceptionType == &typeid(std::bad_alloc));
    void* adj = match_and_begin(ts.header, &typeid(std::exception));
    CHECK(adj != nullptr);
    auto* caught = static_cast<std::exception*>(adj);
    CHECK(std::strcmp(caught->what(), "std::bad_alloc") == 0);
    abi::__cxa_end_catch();
  }
  {
    try_scope ts;
    bool landed = run_catching(ts, [] { abi::__cxa_bad_cast(); });
    CHECK(landed);
    CHECK(ts.header->exceptionType == &typeid(std::bad_cast));
    void* adj = match_and_begin(ts.header, &typeid(std::bad_cast));
    CHECK(adj != nullptr);
    auto* caught = static_cast<std::bad_cast*>(adj);
    CHECK(std::strcmp(caught->what(), "std::bad_cast") == 0);
    abi::__cxa_end_catch();
  }
  {
    try_scope ts;
    bool landed = run_catching(ts, [] { abi::__cxa_bad_typeid(); });
    CHECK(landed);
    CHECK(ts.header->exceptionType == &typeid(std::bad_typeid));
    void* adj = match_and_begin(ts.header, &typeid(std::exception));
    CHECK(adj != nullptr);
    abi::__cxa_end_catch();
  }
  return fails;
}

/* ================================================================
 * Case F1: the primary exception (exception_ptr) handed across
 * threads: captured in a catch on the main thread, rethrown and
 * caught on a weave thread, destroyed exactly once at the far end.
 */
static abi::__cxa_exception* g_handoff_primary = nullptr;
static int g_handoff_type_ok = 0;
static int g_handoff_caught = 0;

static void* handoff_entry(void*) {
  try_scope ts;
  bool landed = run_catching(ts, [] {
    abi::__cxa_rethrow_primary_exception(g_handoff_primary);
  });
  if (!landed) return nullptr;
  void* adj = match_and_begin(ts.header, &typeid(Dog));
  if (adj != nullptr) {
    __atomic_store_n(&g_handoff_caught, 1, __ATOMIC_SEQ_CST);
    if (abi::__cxa_current_exception_type() == &typeid(Dog)) {
      __atomic_store_n(&g_handoff_type_ok, 1, __ATOMIC_SEQ_CST);
    }
    abi::__cxa_end_catch();
  }
  return nullptr;
}

static int case_primary_handoff() {
  int fails = 0;
  const int dog0 = g_dog_dtor;
  try_scope ts;
  Dog* thrown = nullptr;
  bool landed = run_catching(ts, [&] {
    thrown = make_exception<Dog>();
    throw_exception(thrown);
  });
  CHECK(landed);
  void* adj = match_and_begin(ts.header, &typeid(Dog));
  CHECK(adj == static_cast<void*>(thrown));
  abi::lithoabi_eh_header* h = ts.header;
  CHECK(la_atomic_load32(&h->referenceCount) == 1);
  g_handoff_primary = abi::__cxa_current_primary_exception();
  CHECK(g_handoff_primary != nullptr);
  CHECK(la_atomic_load32(&h->referenceCount) == 2);
  abi::__cxa_end_catch();
  /* The pointer's reference keeps the exception alive past the
   * end of its own catch. */
  CHECK(g_dog_dtor == dog0);
  CHECK(la_atomic_load32(&h->referenceCount) == 1);

  lw_descriptor_t d;
  d.entry = &handoff_entry;
  d.arg = nullptr;
  d.stack_size = 0;
  d.name = "handoff";
  lw_thread_t th;
  CHECK(lw_spawn(&d, &th) == LITHOSYS_OK);
  void* rv = nullptr;
  CHECK(lw_join(th, &rv) == LITHOSYS_OK);
  CHECK(g_handoff_caught == 1);
  CHECK(g_handoff_type_ok == 1);
  /* Rethrown, caught, and ended on the far thread: now it dies. */
  CHECK(g_dog_dtor == dog0 + 1);
  g_handoff_primary = nullptr;
  return fails;
}

/* ================================================================
 * Case F1b (audit D-F1): the primary exception rethrown from
 * INSIDE its own handler: capture with current_primary and
 * rethrow through rethrow_primary_exception before any
 * end_catch, the exception_ptr idiom in its sharpest form. At
 * the rethrow the header sits in this thread's caught chain;
 * the rethrow must unlink it, or the outer begin_catch links
 * the header to itself and strands the chain head.
 */
static int case_rethrow_primary_in_handler() {
  int fails = 0;
  const int dog0 = g_dog_dtor;
  try_scope outer;
  Dog* thrown = nullptr;
  abi::lithoabi_eh_header* inner_header = nullptr;
  abi::__cxa_exception* primary = nullptr;
  bool landed_outer = run_catching(outer, [&] {
    try_scope inner;
    bool landed_inner = run_catching(inner, [&] {
      thrown = make_exception<Dog>();
      throw_exception(thrown);
    });
    CHECK(landed_inner);
    void* adj = match_and_begin(inner.header, &typeid(Dog));
    CHECK(adj == static_cast<void*>(thrown));
    inner_header = inner.header;
    primary = abi::__cxa_current_primary_exception();
    CHECK(primary != nullptr);
    CHECK(la_atomic_load32(&inner_header->referenceCount) == 2);
    /* The rethrow: control leaves to the outer frame from here,
     * with the header still linked in this thread's chain. */
    abi::__cxa_rethrow_primary_exception(primary);
  });
  CHECK(landed_outer);
  CHECK(outer.header == inner_header);
  CHECK(uncaught_now() == 1);
  void* adj = match_and_begin(outer.header, &typeid(Dog));
  CHECK(adj == static_cast<void*>(thrown));
  CHECK(g_dog_dtor == dog0); /* alive under the outer handler */
  abi::__cxa_end_catch();
  /* The outer end_catch released the in-flight ownership; the
   * captured primary still holds its own reference, the hold
   * the exception_ptr idiom destroys next. */
  CHECK(la_atomic_load32(&inner_header->referenceCount) == 1);
  CHECK(inner_header->handlerCount == 0);
  CHECK(abi::__cxa_current_exception_type() == nullptr);
  abi::__cxa_decrement_exception_refcount(primary);
  CHECK(g_dog_dtor == dog0 + 1); /* destroyed exactly once */
  /* The chain is clean: a fresh throw / catch cycle on this
   * thread behaves normally. */
  {
    const int cat0 = g_cat_dtor;
    try_scope ts;
    bool landed = run_catching(ts, [&] {
      Cat* cat = make_exception<Cat>();
      throw_exception(cat);
    });
    CHECK(landed);
    void* cadj = match_and_begin(ts.header, &typeid(Cat));
    CHECK(cadj != nullptr);
    abi::__cxa_end_catch();
    CHECK(g_cat_dtor == cat0 + 1);
    CHECK(abi::__cxa_current_exception_type() == nullptr);
    CHECK(uncaught_now() == 0);
  }
  return fails;
}

/* ================================================================
 * Case F2: __cxa_init_primary_exception + the refcount family,
 * without any throw: the last decrement destroys exactly once.
 */
static int case_init_primary() {
  int fails = 0;
  const int dog0 = g_dog_dtor;
  void* mem = abi::__cxa_allocate_exception(sizeof(Dog));
  Dog* obj = new (mem) Dog();
  abi::__cxa_exception* primary = abi::__cxa_init_primary_exception(
      obj, const_cast<std::type_info*>(&typeid(Dog)),
      &destroy_exception_cb<Dog>);
  CHECK(primary != nullptr);
  auto* h = reinterpret_cast<abi::lithoabi_eh_header*>(primary);
  CHECK(la_atomic_load32(&h->referenceCount) == 1);
  CHECK((h->flags & abi::LITHOABI_EH_FLAG_EXTERNAL_OBJECT) != 0);
  CHECK(abi::lithoabi_object_of_header(h) == static_cast<void*>(obj));
  abi::__cxa_increment_exception_refcount(primary);
  CHECK(la_atomic_load32(&h->referenceCount) == 2);
  abi::__cxa_decrement_exception_refcount(primary);
  CHECK(la_atomic_load32(&h->referenceCount) == 1);
  CHECK(g_dog_dtor == dog0);
  abi::__cxa_decrement_exception_refcount(primary);
  CHECK(g_dog_dtor == dog0 + 1);
  return fails;
}

/* ================================================================
 * Case C1: guards, single thread: acquire / release / abort-retry.
 */
static int case_guards_basic() {
  int fails = 0;
  abi::__lithoabi_guard_t guard = 0;
  CHECK(abi::__cxa_guard_acquire(&guard) == 1);
  abi::__cxa_guard_release(&guard);
  CHECK(abi::__cxa_guard_acquire(&guard) == 0); /* already done */
  CHECK((guard & 0xff) == 1);

  abi::__lithoabi_guard_t guard2 = 0;
  CHECK(abi::__cxa_guard_acquire(&guard2) == 1);
  abi::__cxa_guard_abort(&guard2); /* initializer "threw" */
  CHECK((guard2 & 0xff) == 0);
  CHECK(abi::__cxa_guard_acquire(&guard2) == 1); /* retry wins */
  abi::__cxa_guard_release(&guard2);
  CHECK(abi::__cxa_guard_acquire(&guard2) == 0);
  return fails;
}

/* ================================================================
 * Case C1b (review C1 regression): the guard runtime touches exactly
 * sizeof(__lithoabi_guard_t) bytes. On wasm32 the compiler's guard
 * is 4 bytes; the old runtime did 64-bit atomics on it, clobbering
 * the next static and trapping when misaligned. Natively the width
 * is 8, and the bytes after it must stay untouched as well. Also
 * runs a real function-local static through the compiler's own
 * guard path (tests/wasm_guard_check.cpp covers the wasm32 width).
 */
#if defined(__wasm__) || defined(__arm__)
static_assert(sizeof(abi::__lithoabi_guard_t) == 4, "wasm guard width");
#else
static_assert(sizeof(abi::__lithoabi_guard_t) == 8, "Itanium guard width");
#endif
static int g_fls_ctor_runs = 0;
struct FlsProbe {
  int v;
  FlsProbe() : v(42) { ++g_fls_ctor_runs; }
};
static int fls_value() {
  static FlsProbe probe; /* compiler-emitted guard + __cxa_guard_* */
  return probe.v;
}
static int case_guards_width() {
  int fails = 0;
  struct {
    abi::__lithoabi_guard_t g;
    unsigned char after[16];
  } box;
  memset(&box, 0xA5, sizeof box);
  box.g = 0;
  CHECK(abi::__cxa_guard_acquire(&box.g) == 1);
  abi::__cxa_guard_abort(&box.g);
  CHECK(abi::__cxa_guard_acquire(&box.g) == 1);
  abi::__cxa_guard_release(&box.g);
  CHECK(abi::__cxa_guard_acquire(&box.g) == 0);
  for (unsigned i = 0; i < sizeof box.after; ++i) CHECK(box.after[i] == 0xA5);
  CHECK((*reinterpret_cast<unsigned char*>(&box.g) & 1) == 1);
  CHECK(fls_value() == 42);
  CHECK(fls_value() == 42);
  CHECK(g_fls_ctor_runs == 1);
  return fails;
}

/* ================================================================
 * Case C2: guards under thread contention: exactly one winner.
 */
static abi::__lithoabi_guard_t g_race_guard = 0;
static int g_race_inits = 0;
static int g_race_saw_done = 0;

static void* race_entry(void*) {
  if (abi::__cxa_guard_acquire(&g_race_guard) == 1) {
    __atomic_add_fetch(&g_race_inits, 1, __ATOMIC_SEQ_CST);
    /* Hold the initializing state briefly so the other threads
     * really pile onto the wait path. */
    struct timespec nap { 0, 20 * 1000 * 1000 };
    nanosleep(&nap, nullptr);
    abi::__cxa_guard_release(&g_race_guard);
  } else {
    __atomic_add_fetch(&g_race_saw_done, 1, __ATOMIC_SEQ_CST);
  }
  return nullptr;
}

static int case_guards_contention() {
  int fails = 0;
  enum { N = 8 };
  lw_thread_t ths[N];
  lw_descriptor_t d;
  d.entry = &race_entry;
  d.arg = nullptr;
  d.stack_size = 0;
  d.name = "race";
  for (int i = 0; i < N; ++i) {
    CHECK(lw_spawn(&d, &ths[i]) == LITHOSYS_OK);
  }
  for (int i = 0; i < N; ++i) {
    void* rv = nullptr;
    CHECK(lw_join(ths[i], &rv) == LITHOSYS_OK);
  }
  CHECK(g_race_inits == 1);
  CHECK(g_race_saw_done == N - 1);
  return fails;
}

/* ================================================================
 * Case D: atexit ordering, per-DSO finalize, registration during
 * finalize, idempotence; thread_atexit at weave thread exit.
 */
static char g_exit_log[64];
static int g_exit_len = 0;
static void log_letter(void* p) {
  char c = *static_cast<char*>(p);
  if (g_exit_len < 60) g_exit_log[g_exit_len++] = c;
  g_exit_log[g_exit_len] = '\0';
}
static char g_ch_a = 'a', g_ch_b = 'b', g_ch_c = 'c', g_ch_d = 'd',
            g_ch_e = 'e', g_ch_x = 'x', g_ch_y = 'y';
static int g_register_e = 0;
static void log_and_register(void* p) {
  log_letter(p);
  if (g_register_e) {
    g_register_e = 0;
    (void)abi::__cxa_atexit(&log_letter, &g_ch_e, nullptr);
  }
}

static int case_atexit() {
  int fails = 0;
  static char dso_x, dso_y;
  g_exit_len = 0;
  g_exit_log[0] = '\0';
  CHECK(abi::__cxa_atexit(&log_letter, &g_ch_a, &dso_x) == 0);
  CHECK(abi::__cxa_atexit(&log_letter, &g_ch_b, &dso_x) == 0);
  CHECK(abi::__cxa_atexit(&log_letter, &g_ch_c, &dso_y) == 0);
  abi::__cxa_finalize(&dso_x);
  CHECK(std::strcmp(g_exit_log, "ba") == 0); /* reverse order */
  abi::__cxa_finalize(nullptr);
  CHECK(std::strcmp(g_exit_log, "bac") == 0);
  abi::__cxa_finalize(nullptr); /* idempotent: nothing left */
  CHECK(std::strcmp(g_exit_log, "bac") == 0);

  /* Registration during finalize runs in a later pass of the same
   * finalize call. */
  g_register_e = 1;
  CHECK(abi::__cxa_atexit(&log_and_register, &g_ch_d, nullptr) == 0);
  abi::__cxa_finalize(nullptr);
  CHECK(std::strcmp(g_exit_log, "bacde") == 0);

  /* The module's own handle is a valid registration token. */
  CHECK(abi::__cxa_atexit(&log_letter, &g_ch_a, __dso_handle) == 0);
  abi::__cxa_finalize(__dso_handle);
  CHECK(std::strcmp(g_exit_log, "bacdea") == 0);
  return fails;
}

static void* thread_atexit_entry(void*) {
  (void)abi::__cxa_thread_atexit(&log_letter, &g_ch_x, nullptr);
  (void)abi::__cxa_thread_atexit(&log_letter, &g_ch_y, nullptr);
  return nullptr;
}

static int case_thread_atexit() {
  int fails = 0;
  g_exit_len = 0;
  g_exit_log[0] = '\0';
  lw_descriptor_t d;
  d.entry = &thread_atexit_entry;
  d.arg = nullptr;
  d.stack_size = 0;
  d.name = "texit";
  lw_thread_t th;
  CHECK(lw_spawn(&d, &th) == LITHOSYS_OK);
  void* rv = nullptr;
  CHECK(lw_join(th, &rv) == LITHOSYS_OK);
  /* Thread destructors run newest-first at thread exit, before
   * the join completes. */
  CHECK(std::strcmp(g_exit_log, "yx") == 0);
  return fails;
}

/* ================================================================
 * Case G: the personality procedure over synthetic LSDA bytes.
 *
 * The tables below are built by hand in the exact byte layout
 * personality.cc documents: udata4 call-site and TType encodings,
 * the TType table at the end growing backwards from the header's
 * TType base, action chains of sleb128 pairs.
 */
struct byte_buf {
  uint8_t b[256];
  std::size_t n = 0;
  void u8(uint8_t v) { b[n++] = v; }
  void u32(uint32_t v) {
    b[n++] = static_cast<uint8_t>(v);
    b[n++] = static_cast<uint8_t>(v >> 8);
    b[n++] = static_cast<uint8_t>(v >> 16);
    b[n++] = static_cast<uint8_t>(v >> 24);
  }
  void uleb(uintptr_t v) {
    do {
      uint8_t byte = static_cast<uint8_t>(v & 0x7f);
      v >>= 7;
      if (v) byte |= 0x80;
      u8(byte);
    } while (v);
  }
  void sleb(intptr_t v) {
    bool more = true;
    while (more) {
      uint8_t byte = static_cast<uint8_t>(v & 0x7f);
      v >>= 7;
      if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40))) {
        more = false;
      } else {
        byte |= 0x80;
      }
      u8(byte);
    }
  }
};

/* Build the header with a placeholder TType base, then patch it:
 * the TType base is the offset of the END of the whole table. */
static void lsda_header(byte_buf& buf, std::size_t ttype_base_slot) {
  (void)ttype_base_slot;
  buf.u8(0xff); /* landing-pad base: omit */
  buf.u8(0x03); /* TType encoding: udata4 */
  buf.u32(0);   /* TType base: patched by lsda_finish */
  buf.u8(0x03); /* call-site encoding: udata4 */
}
static void lsda_finish(byte_buf& buf) {
  uint32_t total = static_cast<uint32_t>(buf.n);
  buf.b[2] = static_cast<uint8_t>(total);
  buf.b[3] = static_cast<uint8_t>(total >> 8);
  buf.b[4] = static_cast<uint8_t>(total >> 16);
  buf.b[5] = static_cast<uint8_t>(total >> 24);
}

/* The header a __cxa_throw would have produced, minus the throw:
 * the personality tests drive the header directly. */
template <typename E>
abi::lithoabi_eh_header* make_unthrown(E* obj) {
  abi::lithoabi_eh_header* h = abi::lithoabi_header_of_object(obj);
  h->exceptionType = const_cast<std::type_info*>(&typeid(E));
  h->exceptionDestructor = &destroy_exception_cb<E>;
  h->unexpectedHandler = abi::__cxa_unexpected_handler;
  h->terminateHandler = abi::__cxa_terminate_handler;
  h->nextException = nullptr;
  h->handlerCount = 0;
  h->handlerSwitchValue = 0;
  h->actionRecord = nullptr;
  h->languageSpecificData = nullptr;
  h->catchTemp = nullptr;
  h->adjustedPtr = obj;
  return h;
}

static int case_personality() {
  int fails = 0;
  /* LSDA 1: A=catch Dog (ttype 1), B=landing pad with no action,
   * C=catch-all, D=no landing pad. */
  byte_buf buf;
  lsda_header(buf, 0);
  buf.uleb(4 * 13); /* call-site table length */
  buf.u32(0x100); buf.u32(0x10); buf.u32(1); buf.uleb(1); /* A */
  buf.u32(0x200); buf.u32(0x10); buf.u32(1); buf.uleb(0); /* B */
  buf.u32(0x300); buf.u32(0x10); buf.u32(1); buf.uleb(3); /* C */
  buf.u32(0x400); buf.u32(0x10); buf.u32(0); buf.uleb(1); /* D */
  buf.sleb(1); buf.sleb(0); /* record 1 @ action+0: catch ttype 1 */
  buf.sleb(0); buf.sleb(0); /* record 2 @ action+2: catch-all */
  buf.u32(2);               /* ttype entry 2 (second from end) */
  buf.u32(1);               /* ttype entry 1 (last word) */
  lsda_finish(buf);

  const std::type_info* ttypes[2] = {&typeid(Dog), &typeid(Cat)};

  Puppy* puppy = make_exception<Puppy>();
  abi::lithoabi_eh_header* h = make_unthrown(puppy);
  abi::lithoabi_personality_context ctx;
  ctx.lsda = buf.b;
  ctx.lsda_size = buf.n;
  ctx.unwind = &h->unwindHeader;
  ctx.ttype_table = ttypes;
  ctx.ttype_count = 2;

  auto decide = [&](int actions, uintptr_t ip, uint64_t klass) {
    ctx.ip = ip;
    ctx.install_calls = 0;
    ctx.installed_switch = -1;
    ctx.installed_adjusted = nullptr;
    return abi::__gxx_personality_v0(1, actions, klass,
                                     &h->unwindHeader, &ctx);
  };
  const uint64_t cpp_class = abi::lithoabi_cpp_exception_class();
  void* puppy_as_dog = static_cast<void*>(static_cast<Dog*>(puppy));

  /* Search: Puppy thrown, A's clause catches Dog: adjusted. */
  CHECK(decide(abi::_UA_SEARCH_PHASE, 0x104, cpp_class) ==
        abi::_URC_HANDLER_FOUND);
  CHECK(ctx.install_calls == 1);
  CHECK(ctx.installed_switch == 1);
  CHECK(ctx.installed_adjusted == puppy_as_dog);
  CHECK(h->adjustedPtr == puppy_as_dog);
  CHECK(h->handlerSwitchValue == 1);

  /* Search: ip in B (landing pad, no action): continue. */
  CHECK(decide(abi::_UA_SEARCH_PHASE, 0x204, cpp_class) ==
        abi::_URC_CONTINUE_UNWIND);
  CHECK(ctx.install_calls == 0);

  /* Search: ip in C: catch-all selects, object unadjusted. */
  CHECK(decide(abi::_UA_SEARCH_PHASE, 0x304, cpp_class) ==
        abi::_URC_HANDLER_FOUND);
  CHECK(ctx.installed_adjusted == static_cast<void*>(puppy));

  /* Search: ip in D (no landing pad) and outside all sites. */
  CHECK(decide(abi::_UA_SEARCH_PHASE, 0x404, cpp_class) ==
        abi::_URC_CONTINUE_UNWIND);
  CHECK(decide(abi::_UA_SEARCH_PHASE, 0x999, cpp_class) ==
        abi::_URC_CONTINUE_UNWIND);

  /* Foreign exception: no catch matches, but cleanups still run. */
  CHECK(decide(abi::_UA_SEARCH_PHASE, 0x104, 0x1234) ==
        abi::_URC_CONTINUE_UNWIND);
  CHECK(decide(abi::_UA_CLEANUP_PHASE, 0x304, 0x1234) ==
        abi::_URC_HANDLER_FOUND);

  /* Cleanup phase: C has a record (present), B has none. */
  CHECK(decide(abi::_UA_CLEANUP_PHASE, 0x304, cpp_class) ==
        abi::_URC_HANDLER_FOUND);
  CHECK(decide(abi::_UA_CLEANUP_PHASE, 0x204, cpp_class) ==
        abi::_URC_CONTINUE_UNWIND);

  /* Handler frame in phase 2: install context with the same
   * switch value and adjustment the search phase chose. */
  CHECK(decide(abi::_UA_CLEANUP_PHASE | abi::_UA_HANDLER_FRAME,
               0x104, cpp_class) == abi::_URC_INSTALL_CONTEXT);
  CHECK(ctx.installed_switch == 1);
  CHECK(ctx.installed_adjusted == puppy_as_dog);

  puppy->~Puppy();
  abi::__cxa_free_exception(puppy);

  /* A thrown Cat in site A: Dog's clause does not match. */
  Cat* cat = make_exception<Cat>();
  abi::lithoabi_eh_header* ch = make_unthrown(cat);
  ctx.unwind = &ch->unwindHeader;
  ctx.ip = 0x104;
  ctx.install_calls = 0;
  CHECK(abi::__gxx_personality_v0(1, abi::_UA_SEARCH_PHASE, cpp_class,
                                  &ch->unwindHeader, &ctx) ==
        abi::_URC_CONTINUE_UNWIND);
  cat->~Cat();
  abi::__cxa_free_exception(cat);

  /* LSDA 2: one site whose only record is a filter listing Dog.
   * Thrown Cat: the filter selects. Thrown Dog: it does not. */
  byte_buf f;
  lsda_header(f, 0);
  f.uleb(13);
  f.u32(0x100); f.u32(0x10); f.u32(1); f.uleb(1);
  f.sleb(-5); f.sleb(0);   /* record @0: filter, list at action+5 */
  f.u8(0); f.u8(0); f.u8(0);
  f.uleb(1); f.uleb(0);    /* list @5: ttype 1, terminator */
  f.u32(1);                /* ttype entry 1 */
  lsda_finish(f);
  ctx.lsda = f.b;
  ctx.lsda_size = f.n;
  ctx.ttype_count = 2;

  Cat* cat2 = make_exception<Cat>();
  abi::lithoabi_eh_header* c2h = make_unthrown(cat2);
  ctx.unwind = &c2h->unwindHeader;
  ctx.ip = 0x104;
  CHECK(abi::__gxx_personality_v0(1, abi::_UA_SEARCH_PHASE, cpp_class,
                                  &c2h->unwindHeader, &ctx) ==
        abi::_URC_HANDLER_FOUND);
  CHECK(c2h->actionRecord != nullptr);
  cat2->~Cat();
  abi::__cxa_free_exception(cat2);

  Dog* dog2 = make_exception<Dog>();
  abi::lithoabi_eh_header* d2h = make_unthrown(dog2);
  ctx.unwind = &d2h->unwindHeader;
  CHECK(abi::__gxx_personality_v0(1, abi::_UA_SEARCH_PHASE, cpp_class,
                                  &d2h->unwindHeader, &ctx) ==
        abi::_URC_CONTINUE_UNWIND);
  dog2->~Dog();
  abi::__cxa_free_exception(dog2);
  return fails;
}

/* ================================================================
 * Case G2 (audit D-F2 / D-F3): the personality procedure over
 * hostile LSDA shapes. A cyclic action chain must return with
 * no selection instead of hanging the unwinder, and an action
 * offset that wraps the entry pointer below the LSDA must be
 * rejected before any pointer is formed, never read as a
 * record out of unrelated memory.
 */
static uint8_t g_fake_catchall_record[2] = {0x00, 0x00};

static int case_personality_hardening() {
  int fails = 0;
  const std::type_info* ttypes[2] = {&typeid(Dog), &typeid(Cat)};
  const uint64_t cpp_class = abi::lithoabi_cpp_exception_class();
  Dog* dog = make_exception<Dog>();
  abi::lithoabi_eh_header* h = make_unthrown(dog);
  abi::lithoabi_personality_context ctx;
  ctx.unwind = &h->unwindHeader;
  ctx.ttype_table = ttypes;
  ctx.ttype_count = 2;
  ctx.ip = 0x104;

  /* Cycle: record A (ttype 2 = Cat, disp +1 to B) and record B
   * (ttype 2, disp -3 back to A) point at each other; the thrown
   * Dog matches neither. The search must terminate with no
   * selection. On the unfixed code this case never returns. */
  {
    byte_buf buf;
    lsda_header(buf, 0);
    buf.uleb(13);
    buf.u32(0x100); buf.u32(0x10); buf.u32(1); buf.uleb(1);
    buf.sleb(2); buf.sleb(1);  /* A @ action+0 */
    buf.sleb(2); buf.sleb(-3); /* B @ action+2 */
    buf.u32(2);
    buf.u32(1);
    lsda_finish(buf);
    ctx.lsda = buf.b;
    ctx.lsda_size = buf.n;
    ctx.install_calls = 0;
    CHECK(abi::__gxx_personality_v0(1, abi::_UA_SEARCH_PHASE, cpp_class,
                                    &h->unwindHeader, &ctx) ==
          abi::_URC_CONTINUE_UNWIND);
    CHECK(ctx.install_calls == 0);
  }

  /* Wrap: the call-site action offset is chosen so that
   * action_table + disp - 1 lands exactly on a catch-all record
   * held in static storage BELOW the LSDA. The offset travels
   * as a fixed 10-byte ULEB so the call-site table length is
   * known before the value is: 7 header bytes + 1 length byte +
   * the 22-byte record put the action table at buf.b + 30. */
  {
    byte_buf buf;
    lsda_header(buf, 0);
    buf.uleb(12 + 10); /* one call-site record, 10-byte action ULEB */
    buf.u32(0x100); buf.u32(0x10); buf.u32(1);
    const uint8_t* action_table = buf.b + 30;
    uintptr_t disp = static_cast<uintptr_t>(
        reinterpret_cast<uintptr_t>(g_fake_catchall_record) -
        reinterpret_cast<uintptr_t>(action_table) + 1);
    uintptr_t v = disp;
    for (int i = 0; i < 10; i++) {
      uint8_t byte = static_cast<uint8_t>(v & 0x7f);
      v >>= 7;
      if (i < 9) byte |= 0x80;
      buf.u8(byte);
    }
    buf.u32(2);
    buf.u32(1);
    lsda_finish(buf);
    ctx.lsda = buf.b;
    ctx.lsda_size = buf.n;
    ctx.install_calls = 0;
    CHECK(abi::__gxx_personality_v0(1, abi::_UA_SEARCH_PHASE, cpp_class,
                                    &h->unwindHeader, &ctx) ==
          abi::_URC_CONTINUE_UNWIND);
    CHECK(ctx.install_calls == 0);
  }

  dog->~Dog();
  abi::__cxa_free_exception(dog);
  return fails;
}

/* ================================================================
 * Case H: dynamic_cast through the compiler's own call sites (the
 * expressions below emit calls to LithoAbi's __dynamic_cast).
 */
struct A0 { virtual ~A0(); };
A0::~A0() = default;
struct B0 : A0 { int b = 0; };
struct C0 : A0 { int c = 0; };
struct D0 : B0, C0 { int d = 0; };
struct M0 { virtual ~M0(); };
M0::~M0() = default;
struct D2 : B0, C0, M0 { int e = 0; };
struct VBase { virtual ~VBase(); };
VBase::~VBase() = default;
struct Left : virtual VBase { int l = 0; };
struct Right : virtual VBase { int r = 0; };
struct Diamond : Left, Right { int dm = 0; };

static int case_dynamic_casts() {
  int fails = 0;
  Dog dog;
  Animal* a = &dog;
  CHECK(dynamic_cast<Dog*>(a) == &dog);
  CHECK(dynamic_cast<Puppy*>(a) == nullptr);
  CHECK(dynamic_cast<void*>(a) == static_cast<void*>(&dog));
  CHECK(dynamic_cast<OtherBase*>(a) == nullptr);

  Puppy puppy;
  OtherBase* o = &puppy;
  CHECK(dynamic_cast<Dog*>(o) == static_cast<Dog*>(&puppy));
  CHECK(dynamic_cast<Animal*>(o) ==
        static_cast<Animal*>(static_cast<Dog*>(&puppy)));
  CHECK(dynamic_cast<Puppy*>(o) == &puppy);

  /* Cross-cast in a non-virtual diamond works. The shared base
   * cast from one branch pointer follows the standard's clause
   * order: the dynamic-type route finds A0 ambiguous, and the
   * static-type route (A0 is B0's own public base) answers with
   * this branch's A0 sub-object. Cast from an UNRELATED branch of
   * the same object, A0 has no static route and the ambiguity
   * fails the cast, as the host runtime also rules (both shapes
   * were probed against libstdc++ during the test pass). */
  D0 d0;
  B0* b = &d0;
  CHECK(dynamic_cast<C0*>(b) == static_cast<C0*>(&d0));
  CHECK(dynamic_cast<A0*>(b) == static_cast<A0*>(b));
  CHECK(dynamic_cast<void*>(b) == static_cast<void*>(&d0));
  D2 d2;
  M0* m = &d2;
  CHECK(dynamic_cast<A0*>(m) == nullptr);
  CHECK(dynamic_cast<B0*>(m) == static_cast<B0*>(&d2));

  /* Virtual diamond: the shared base is unique and reachable. */
  Diamond dm;
  Left* l = &dm;
  CHECK(dynamic_cast<Right*>(l) == static_cast<Right*>(&dm));
  CHECK(dynamic_cast<VBase*>(l) == static_cast<VBase*>(&dm));
  CHECK(dynamic_cast<Diamond*>(l) == &dm);

  /* Upcast within a standalone object; unrelated type fails. */
  Dog solo;
  Dog* sd = &solo;
  CHECK(dynamic_cast<Animal*>(sd) == static_cast<Animal*>(&solo));
  CHECK(dynamic_cast<Cat*>(static_cast<Animal*>(sd)) == nullptr);

  Animal* none = nullptr;
  CHECK(dynamic_cast<Dog*>(none) == nullptr);
  return fails;
}

/* ================================================================
 * Case I: terminate paths with a recording handler installed.
 * The handler longjmps back to the test (a handler that returns
 * would be aborted by std::terminate itself, which the death
 * cases exercise separately with the default handler).
 */
static jmp_buf g_term_jmp;
static int g_term_calls = 0;
static void recording_terminate() {
  __atomic_add_fetch(&g_term_calls, 1, __ATOMIC_SEQ_CST);
  longjmp(g_term_jmp, 1);
}

static int case_terminate_paths() {
  int fails = 0;
  std::terminate_handler old = std::set_terminate(&recording_terminate);
  CHECK(old != nullptr);
  CHECK(std::get_terminate() == &recording_terminate);
  g_term_calls = 0;

  if (setjmp(g_term_jmp) == 0) {
    abi::__cxa_pure_virtual();
    CHECK(!"__cxa_pure_virtual returned");
  }
  CHECK(g_term_calls == 1);

  if (setjmp(g_term_jmp) == 0) {
    abi::__cxa_deleted_virtual();
    CHECK(!"__cxa_deleted_virtual returned");
  }
  CHECK(g_term_calls == 2);

  /* __cxa_call_terminate on a real in-flight exception: it enters
   * the catch, then terminates; the exception is still ours to
   * dispose of afterwards. */
  const int dog0 = g_dog_dtor;
  try_scope ts;
  bool landed = run_catching(ts, [&] {
    Dog* d = make_exception<Dog>();
    throw_exception(d);
  });
  CHECK(landed);
  if (setjmp(g_term_jmp) == 0) {
    abi::__cxa_call_terminate(&ts.header->unwindHeader);
    CHECK(!"__cxa_call_terminate returned");
  }
  CHECK(g_term_calls == 3);
  CHECK(abi::__cxa_current_exception_type() == &typeid(Dog));
  abi::__cxa_end_catch();
  CHECK(g_dog_dtor == dog0 + 1);

  std::set_terminate(old);
  CHECK(std::get_terminate() == old);
  return fails;
}

/* ================================================================
 * Case B: the emergency pool under a starved heap.
 */
static char g_tiny_arena[64 * 1024];
static void* g_fill[8192];

static wh_heap_t* make_tiny_heap(int& fails) {
  wh_options_t opts;
  opts.grow = nullptr;
  opts.grow_user = nullptr;
  opts.lock = &heap_lock;
  opts.unlock = &heap_unlock;
  opts.lock_user = nullptr;
  wh_heap_t* tiny = wh_heap_create(g_tiny_arena, sizeof(g_tiny_arena),
                                   &opts);
  CHECK(tiny != nullptr);
  return tiny;
}

static int starve_heap() {
  int nf = 0;
  while (nf < 8192) {
    void* p = malloc(96);
    if (!p) break;
    g_fill[nf++] = p;
  }
  return nf;
}

static void unstarve_heap(int nf) {
  for (int i = 0; i < nf; ++i) free(g_fill[i]);
}

static int case_emergency_pool() {
  int fails = 0;
  wh_heap_t* tiny = make_tiny_heap(fails);
  if (!tiny) return fails;
  wh_set_default_heap(tiny);
  int nf = starve_heap();
  CHECK(nf > 0);
  CHECK(malloc(96) == nullptr); /* really starved */

  void* held[7];
  for (int i = 0; i < 7; ++i) {
    held[i] = abi::__cxa_allocate_exception(200);
    CHECK(held[i] != nullptr);
    abi::lithoabi_eh_header* h = abi::lithoabi_header_of_object(held[i]);
    CHECK((h->flags & abi::LITHOABI_EH_FLAG_FROM_POOL) != 0);
  }

  /* The reserved slot serves bad_alloc while all general slots
   * are held. */
  {
    try_scope ts;
    bool landed = run_catching(ts, [] { abi::lithoabi_throw_bad_alloc(); });
    CHECK(landed);
    abi::lithoabi_eh_header* h = ts.header;
    CHECK((h->flags & abi::LITHOABI_EH_FLAG_FROM_POOL) != 0);
    void* adj = match_and_begin(ts.header, &typeid(std::exception));
    CHECK(adj != nullptr);
    CHECK(std::strcmp(static_cast<std::exception*>(adj)->what(),
                      "std::bad_alloc") == 0);
    abi::__cxa_end_catch();
  }

  /* A general allocation may borrow the reserved slot as its last
   * resort. */
  void* borrower = abi::__cxa_allocate_exception(200);
  CHECK(borrower != nullptr);
  abi::lithoabi_eh_header* bh = abi::lithoabi_header_of_object(borrower);
  CHECK((bh->flags & abi::LITHOABI_EH_FLAG_FROM_POOL) != 0);

  abi::__cxa_free_exception(borrower);
  for (int i = 0; i < 7; ++i) abi::__cxa_free_exception(held[i]);
  unstarve_heap(nf);
  wh_heap_destroy(tiny);
  wh_set_default_heap(g_heap);

  /* Back on the healthy heap, allocations are heap allocations. */
  void* obj = abi::__cxa_allocate_exception(64);
  CHECK(obj != nullptr);
  CHECK((abi::lithoabi_header_of_object(obj)->flags &
         abi::LITHOABI_EH_FLAG_FROM_POOL) == 0);
  abi::__cxa_free_exception(obj);
  return fails;
}

/* ================================================================
 * Death cases. Each runs in a re-executed child and must die of
 * SIGABRT (the native shape of lithosys_abort).
 */
[[noreturn]] static void die_scenario(int which) {
  switch (which) {
    case 1: { /* uncaught throw: no frame on the stack */
      Dog* d = make_exception<Dog>();
      throw_exception(d);
    }
    case 2: { /* recursive guard initialization */
      abi::__lithoabi_guard_t guard = 0;
      (void)abi::__cxa_guard_acquire(&guard);
      (void)abi::__cxa_guard_acquire(&guard);
      break;
    }
    case 3: { /* heap starved, pool exhausted: allocate once more */
      int fails = 0;
      wh_heap_t* tiny = make_tiny_heap(fails);
      if (!tiny) _exit(4);
      wh_set_default_heap(tiny);
      (void)starve_heap();
      for (int i = 0; i < 8; ++i) {
        (void)abi::__cxa_allocate_exception(200);
      }
      (void)abi::__cxa_allocate_exception(200); /* this one dies */
      break;
    }
    case 5: { /* audit D-F4 / E-7: a thrown size within the header
               * size of the size_t maximum must be refused into
               * the terminate path, never wrapped into a small
               * allocation. Without the guard the wrapped total
               * allocates happily and this scenario exits 3. */
      (void)abi::__cxa_allocate_exception(~(std::size_t)0 - 1);
      break;
    }
    case 4: /* pure virtual with the default terminate handler */
      abi::__cxa_pure_virtual();
    default:
      break;
  }
  _exit(3); /* reached only if the scenario failed to die */
}

static int run_death(int which) {
  pid_t pid = fork();
  if (pid == 0) {
    char num[2] = {static_cast<char>('0' + which), '\0'};
    execl("/proc/self/exe", "lithoabi_test", "--die", num,
          static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0) return 1;
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return 1;
  return (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) ? 0 : 1;
}

static int case_death_tests() {
  int fails = 0;
  for (int which = 1; which <= 5; ++which) {
    int rc = run_death(which);
    if (rc != 0) {
      std::printf("  FAIL check: death case %d did not die of "
                  "SIGABRT\n", which);
      ++fails;
    }
  }
  return fails;
}

/* ================================================================
 * Setup and main.
 */
int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  alarm(180); /* a hang must die loudly, not forever */

  const std::size_t arena_bytes = 16u * 1024u * 1024u;
  void* arena = mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (arena == MAP_FAILED) {
    std::printf("FAIL: MAP_32BIT arena unavailable\n");
    return 1;
  }
  wh_options_t opts;
  opts.grow = nullptr;
  opts.grow_user = nullptr;
  opts.lock = &heap_lock;
  opts.unlock = &heap_unlock;
  opts.lock_user = nullptr;
  g_heap = wh_heap_create(arena, arena_bytes, &opts);
  if (!g_heap) {
    std::printf("FAIL: wh_heap_create\n");
    return 1;
  }
  wh_set_default_heap(g_heap);
  if (lw_main_thread_init() != LITHOSYS_OK) {
    std::printf("FAIL: lw_main_thread_init\n");
    return 1;
  }

  if (argc >= 3 && std::strcmp(argv[1], "--die") == 0) {
    die_scenario(argv[2][0] - '0');
  }

  std::printf("setup: heap over MAP_32BIT arena, main thread "
              "attached\n");
  RUN_CASE(case_alloc_free_header);
  RUN_CASE(case_eh_exact_catch);
  RUN_CASE(case_eh_base_catch_adjust);
  RUN_CASE(case_eh_catch_all);
  RUN_CASE(case_eh_rethrow);
  RUN_CASE(case_eh_own_throws);
  RUN_CASE(case_primary_handoff);
  RUN_CASE(case_rethrow_primary_in_handler);
  RUN_CASE(case_init_primary);
  RUN_CASE(case_guards_basic);
  RUN_CASE(case_guards_width);
  RUN_CASE(case_guards_contention);
  RUN_CASE(case_atexit);
  RUN_CASE(case_thread_atexit);
  RUN_CASE(case_personality);
  RUN_CASE(case_personality_hardening);
  RUN_CASE(case_dynamic_casts);
  RUN_CASE(case_terminate_paths);
  RUN_CASE(case_emergency_pool);
  RUN_CASE(case_death_tests);

  std::printf("%s: lithoabi_test (%d failing checks)\n",
              g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
  return g_failures == 0 ? 0 : 1;
}
