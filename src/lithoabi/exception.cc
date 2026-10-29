/* exception.cc ... exception allocation, the emergency pool, the
 * refcount discipline, per-thread eh globals, and the primary
 * exception family (the ABI side of std::exception_ptr).
 *
 * Lifetime rules (the whole file in one paragraph): allocation
 * charges referenceCount = 1, owned by the in-flight throw.
 * __cxa_begin_catch borrows that ownership for the handler (it is
 * tracked separately in handlerCount); __cxa_end_catch releases the
 * in-flight ownership for the innermost handled exception;
 * __cxa_rethrow hands the ownership back to the in-flight state
 * without touching the count. std::exception_ptr's primitives add
 * and remove owners of their own. One helper, lithoabi_maybe_release,
 * is the only place an exception is destroyed and freed: it fires
 * exactly when the count reaches 0 while no handler holds the
 * exception. Every decrement site ends by calling it.
 */

#include <stdlib.h> /* LithoLibC: the allocation family */
#include <cstring>

#include "pthread.h" /* LithoThread: key + once for the globals TLS */

#include "lithoabi_internal.h"

namespace __cxxabiv1 {

/* ----------------------------------------------------------------
 * Emergency pool.
 *
 * When LithoLibC malloc cannot serve an exception allocation (heap
 * exhausted, or malloc itself is what just failed), the ABI still
 * has to throw: the allocation the caller is performing has usually
 * failed BECAUSE there is no memory, and the exception it is
 * throwing is most often std::bad_alloc. So a static pool of
 * fixed-size slots backs the allocation. Slots serve any exception
 * whose object fits the payload; the FINAL slot is reserved for the
 * bad_alloc path (allocations that arrive through
 * lithoabi_allocate_exception_for_bad_alloc, the entry under
 * LithoAbi's own bad_alloc thrower, which the stdlib piece's
 * operator new failure path will call) and is lent to a general
 * allocation only as the last resort before terminate, because a
 * terminated process is strictly worse than a borrowed reservation.
 *
 * Sizing: engine exceptions are small objects (a vtable pointer and
 * a message pointer or a short string buffer; cRpcError and the
 * std::runtime_error family are all well under 100 bytes of object).
 * 256 bytes of payload covers them with room to spare, and 8 slots
 * cover nested failure storms (a throw inside a catch inside a
 * throw, each holding one live exception). The pool costs
 * 8 * (header + 256) bytes of static storage, about 2.6 KB.
 *
 * Slot state lives in one atomic bitmap (bit i set: slot i busy),
 * so pool allocation needs no lock and is safe from any thread and
 * from inside a failing allocator's call stack.
 */
constexpr std::size_t LITHOABI_EMERGENCY_PAYLOAD = 256;
constexpr std::size_t LITHOABI_EMERGENCY_SLOTS = 8;
constexpr std::size_t LITHOABI_EMERGENCY_SLOT_BYTES =
    ((sizeof(lithoabi_eh_header) + LITHOABI_EMERGENCY_PAYLOAD +
      (LITHOABI_EH_ALIGNMENT - 1)) &
     ~(LITHOABI_EH_ALIGNMENT - 1));

struct lithoabi_emergency_slot {
  alignas(LITHOABI_EH_ALIGNMENT) unsigned char bytes[LITHOABI_EMERGENCY_SLOT_BYTES];
};
static lithoabi_emergency_slot lithoabi_pool[LITHOABI_EMERGENCY_SLOTS];
static uint32_t lithoabi_pool_busy = 0; /* atomic bitmap */

static lithoabi_eh_header* lithoabi_pool_take(bool for_bad_alloc) {
  const std::size_t general = LITHOABI_EMERGENCY_SLOTS - 1;
  /* Pass 1: the slots this caller is entitled to. The bad_alloc
   * path starts at the reserved slot; the general path scans the
   * general slots. Pass 2 (general path only, and only when pass 1
   * found nothing): the reserved slot as a last resort. */
  for (int pass = 0; pass < 2; ++pass) {
    std::size_t begin, end;
    if (for_bad_alloc) {
      if (pass == 1) break;
      begin = general;
      end = LITHOABI_EMERGENCY_SLOTS;
    } else if (pass == 0) {
      begin = 0;
      end = general;
    } else {
      begin = general;
      end = LITHOABI_EMERGENCY_SLOTS;
    }
    for (std::size_t i = begin; i < end; ++i) {
      uint32_t bit = 1u << i;
      uint32_t seen = la_atomic_load32(&lithoabi_pool_busy);
      while (!(seen & bit)) {
        if (la_atomic_cas32(&lithoabi_pool_busy, &seen, seen | bit)) {
          auto* header = reinterpret_cast<lithoabi_eh_header*>(
              lithoabi_pool[i].bytes);
          std::memset(header, 0, sizeof(*header));
          header->flags |= LITHOABI_EH_FLAG_FROM_POOL;
          header->referenceCount = 1;
          return header;
        }
      }
    }
  }
  return nullptr;
}

static void lithoabi_pool_give(lithoabi_eh_header* header) {
  const auto* p = reinterpret_cast<const unsigned char*>(header);
  std::size_t i = (p - lithoabi_pool[0].bytes) / LITHOABI_EMERGENCY_SLOT_BYTES;
  uint32_t bit = 1u << i;
  uint32_t seen = la_atomic_load32(&lithoabi_pool_busy);
  while ((seen & bit) &&
         !la_atomic_cas32(&lithoabi_pool_busy, &seen, seen & ~bit)) {
  }
}

/* ----------------------------------------------------------------
 * Shared release machinery.
 */

static void lithoabi_destroy_and_free(lithoabi_eh_header* header) {
  void* object = lithoabi_object_of_header(header);
  if (header->exceptionDestructor) header->exceptionDestructor(object);
  if (header->flags & LITHOABI_EH_FLAG_EXTERNAL_OBJECT) {
    /* A primary exception whose object lives in its own
     * __cxa_allocate_exception allocation: release that allocation
     * too (its header space in front of the object was never
     * initialized; __cxa_free_exception computes and releases it). */
    __cxa_free_exception(object);
    free(header);
    return;
  }
  if (header->flags & LITHOABI_EH_FLAG_FROM_POOL) {
    lithoabi_pool_give(header);
    return;
  }
  free(header);
}

/* Destroy and free iff the refcount has reached 0 and no handler
 * currently holds the exception. Returns true when it fired. */
bool lithoabi_maybe_release(lithoabi_eh_header* header) {
  if (la_atomic_load32(&header->referenceCount) == 0 &&
      header->handlerCount == 0) {
    lithoabi_destroy_and_free(header);
    return true;
  }
  return false;
}

/* ----------------------------------------------------------------
 * Allocation.
 */

static lithoabi_eh_header* lithoabi_allocate_header_for(
    std::size_t thrown_size, bool for_bad_alloc, bool* out_from_pool) {
  /* Audit fix D-F4 / E-7: the header size addition must not wrap.
   * A thrown_size within the header size of the size_t maximum
   * would wrap the total to a small allocation that the caller's
   * construction then overruns. No real type approaches this; the
   * guard keeps the public entry honest anyway, and nullptr runs
   * into the caller's terminate path, the same end as exhaustion. */
  if (thrown_size > ~(std::size_t)0 - lithoabi_header_size()) {
    return nullptr;
  }
  const std::size_t total = lithoabi_header_size() + thrown_size;
  if (void* raw = malloc(total)) {
    auto* header = static_cast<lithoabi_eh_header*>(raw);
    std::memset(header, 0, sizeof(*header));
    header->referenceCount = 1;
    *out_from_pool = false;
    return header;
  }
  if (thrown_size <= LITHOABI_EMERGENCY_PAYLOAD) {
    if (lithoabi_eh_header* header = lithoabi_pool_take(for_bad_alloc)) {
      *out_from_pool = true;
      return header;
    }
  }
  return nullptr;
}

/* Internal: the allocation entry the bad_alloc throws use, so the
 * reserved slot discipline can tell them apart. */
void* lithoabi_allocate_exception_impl(std::size_t thrown_size,
                                       bool for_bad_alloc) {
  bool from_pool = false;
  lithoabi_eh_header* header =
      lithoabi_allocate_header_for(thrown_size, for_bad_alloc, &from_pool);
  if (!header) {
    /* The ABI forbids returning NULL here: an allocation routine
     * that cannot even produce the failure exception has nothing
     * left but the terminate path. */
    lithoabi_terminate_now(
        "lithoabi: exception allocation failed (heap and emergency "
        "pool exhausted)\n");
  }
  (void)from_pool;
  /* The unwind header is the part other components key on: set its
   * class and cleanup once, at birth. */
  header->unwindHeader.exception_class = lithoabi_cpp_exception_class();
  header->unwindHeader.exception_cleanup = nullptr;
  header->adjustedPtr =
      reinterpret_cast<char*>(header) + lithoabi_header_size();
  return header->adjustedPtr;
}

void* __cxa_allocate_exception(std::size_t thrown_size) {
  return lithoabi_allocate_exception_impl(thrown_size, false);
}

/* The bad_alloc path (eh_runtime.cc throws through this). */
void* lithoabi_allocate_exception_for_bad_alloc(std::size_t thrown_size) {
  return lithoabi_allocate_exception_impl(thrown_size, true);
}

void __cxa_free_exception(void* thrown_exception) {
  if (!thrown_exception) return;
  lithoabi_eh_header* header = lithoabi_header_of_object(thrown_exception);
  if (header->flags & LITHOABI_EH_FLAG_FROM_POOL) {
    lithoabi_pool_give(header);
    return;
  }
  free(header);
}

__cxa_exception* __cxa_init_primary_exception(void* object,
                                              std::type_info* rtti,
                                              __cxa_destructor_fn destructor) {
  auto* header = static_cast<lithoabi_eh_header*>(malloc(sizeof(lithoabi_eh_header)));
  if (!header) {
    lithoabi_terminate_now(
        "lithoabi: primary exception header allocation failed\n");
  }
  std::memset(header, 0, sizeof(*header));
  header->referenceCount = 1;
  header->flags |= LITHOABI_EH_FLAG_EXTERNAL_OBJECT;
  header->primaryObject = object;
  header->exceptionType = rtti;
  header->exceptionDestructor = destructor;
  header->adjustedPtr = object;
  header->unwindHeader.exception_class = lithoabi_cpp_exception_class();
  return reinterpret_cast<__cxa_exception*>(header);
}

/* ----------------------------------------------------------------
 * Per-thread eh globals, over a LithoThread key.
 */

static pthread_key_t lithoabi_globals_key;
static pthread_once_t lithoabi_globals_once = PTHREAD_ONCE_INIT;

static void lithoabi_globals_destroy(void* p) {
  auto* globals = static_cast<__cxa_eh_globals*>(p);
  /* A thread that exits still holding caught exceptions leaks their
   * headers on purpose: running destructors here could touch state
   * the thread has already torn down. Nothing in the engine holds a
   * caught exception across thread exit. */
  free(globals);
}
static void lithoabi_globals_make_key() {
  (void)pthread_key_create(&lithoabi_globals_key, &lithoabi_globals_destroy);
}

__cxa_eh_globals* __cxa_get_globals() {
  (void)pthread_once(&lithoabi_globals_once, &lithoabi_globals_make_key);
  if (void* p = pthread_getspecific(lithoabi_globals_key)) {
    return static_cast<__cxa_eh_globals*>(p);
  }
  auto* globals = static_cast<__cxa_eh_globals*>(calloc(1, sizeof(__cxa_eh_globals)));
  if (!globals) {
    lithoabi_terminate_now("lithoabi: eh globals allocation failed\n");
  }
  (void)pthread_setspecific(lithoabi_globals_key, globals);
  return globals;
}

__cxa_eh_globals* __cxa_get_globals_fast() {
  /* No once, no creation: before the key exists this thread cannot
   * have globals, so NULL is the honest answer. pthread_once in
   * __cxa_get_globals is what creates the key; until it has run,
   * pthread_getspecific on an uncreated key is not safe to call, so
   * the fast path peeks at the once control's state word instead.
   * LithoThread's pthread_once_t is one uint32_t whose completed
   * value the piece documents internally; to stay clear of that
   * private encoding, the fast path simply asks whether this thread
   * already has a value through a cached key-valid flag. */
  static uint32_t key_ready = 0; /* atomic */
  if (!la_atomic_load32(&key_ready)) {
    (void)__cxa_get_globals();
    la_atomic_store32(&key_ready, 1);
    return __cxa_get_globals_fast();
  }
  return static_cast<__cxa_eh_globals*>(
      pthread_getspecific(lithoabi_globals_key));
}

/* ----------------------------------------------------------------
 * Primary exception refcount family.
 */

__cxa_exception* __cxa_current_primary_exception() {
  __cxa_eh_globals* globals = __cxa_get_globals_fast();
  if (!globals) return nullptr;
  lithoabi_eh_header* header = reinterpret_cast<lithoabi_eh_header*>(
      globals->caughtExceptions);
  if (!header) return nullptr;
  la_atomic_fetch_add32(&header->referenceCount, 1);
  return reinterpret_cast<__cxa_exception*>(header);
}

void __cxa_increment_exception_refcount(__cxa_exception* primary) {
  if (!primary) return;
  auto* header = reinterpret_cast<lithoabi_eh_header*>(primary);
  la_atomic_fetch_add32(&header->referenceCount, 1);
}

void __cxa_decrement_exception_refcount(__cxa_exception* primary) {
  if (!primary) return;
  auto* header = reinterpret_cast<lithoabi_eh_header*>(primary);
  la_atomic_fetch_sub32(&header->referenceCount, 1);
  (void)lithoabi_maybe_release(header);
}

} /* namespace __cxxabiv1 */
