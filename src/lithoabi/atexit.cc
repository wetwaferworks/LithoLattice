/* atexit.cc ... __cxa_atexit, __cxa_finalize, __dso_handle, and
 * __cxa_thread_atexit over LithoWeave's destructor registry.
 *
 * Process exit registrations live in one list, newest first (so a
 * plain walk of a detached batch is already reverse registration
 * order). Finalize runs in passes, the same discipline LithoWeave
 * set for TLS destructors: one pass detaches the list as it stands
 * and runs the entries for the requested DSO (all of them for a
 * NULL handle); entries registered DURING a pass land in the fresh
 * list and run in the next pass; passes repeat while registrations
 * keep arriving, up to a cap, after which still-unrun entries for
 * the requested DSO stay registered (documented drop-by-cap, the
 * same trade LithoWeave documents for its pass limit: an exit that
 * never finishes is worse than a bounded one). Entries for other
 * DSO handles are deferred untouched, order preserved.
 *
 * Thread exit registrations are not kept here at all: they ride
 * LithoWeave's lw_thread_atexit, which owns the per-thread lists
 * and the exit-time pass discipline. LithoAbi's part is the
 * adapter: the ABI's destructor shape and the weave's callback
 * shape are both void (*)(void*), so the adapter only has to carry
 * the registration record and free it after the call.
 */

#include <stdlib.h> /* LithoLibC */

#include "pthread.h"    /* LithoThread */
#include "lithoweave.h" /* lw_thread_atexit */

#include "lithoabi_internal.h"

extern "C" {
/* Weak on purpose: a hosted C runtime's crtbegin provides this
 * same token on native links and must win there (every registrant
 * in the process has to agree on one handle value), while on the
 * wasm target there is no crtbegin and this definition is the
 * only one. Either way its value is its own address, which is the
 * whole contract. */
__attribute__((weak)) void* __dso_handle = &__dso_handle;
}

namespace __cxxabiv1 {
namespace {

struct atexit_node {
  void (*fn)(void*);
  void* arg;
  void* dso;
  atexit_node* next;
};

pthread_mutex_t atexit_mutex = PTHREAD_MUTEX_INITIALIZER;
atexit_node* atexit_list = nullptr;
uint32_t atexit_registrations = 0; /* under atexit_mutex */

constexpr int ATEXIT_PASS_LIMIT = 8;

} /* namespace */

int __cxa_atexit(void (*fn)(void*), void* arg, void* dso_handle) {
  if (!fn) return -1;
  auto* node = static_cast<atexit_node*>(malloc(sizeof(atexit_node)));
  if (!node) return -1;
  node->fn = fn;
  node->arg = arg;
  node->dso = dso_handle;
  (void)pthread_mutex_lock(&atexit_mutex);
  node->next = atexit_list;
  atexit_list = node;
  atexit_registrations += 1;
  (void)pthread_mutex_unlock(&atexit_mutex);
  return 0;
}

void __cxa_finalize(void* dso_handle) {
  for (int pass = 0; pass < ATEXIT_PASS_LIMIT; ++pass) {
    uint32_t registrations_before;
    atexit_node* batch;
    (void)pthread_mutex_lock(&atexit_mutex);
    registrations_before = atexit_registrations;
    batch = atexit_list;
    atexit_list = nullptr;
    (void)pthread_mutex_unlock(&atexit_mutex);
    if (!batch) return;

    atexit_node* deferred = nullptr;
    atexit_node** deferred_tail = &deferred;
    for (atexit_node* node = batch; node;) {
      atexit_node* next = node->next;
      if (!dso_handle || node->dso == dso_handle) {
        node->fn(node->arg);
        free(node);
      } else {
        node->next = nullptr;
        *deferred_tail = node;
        deferred_tail = &node->next;
      }
      node = next;
    }

    (void)pthread_mutex_lock(&atexit_mutex);
    if (deferred) {
      if (!atexit_list) {
        atexit_list = deferred;
      } else {
        atexit_node* tail = atexit_list;
        while (tail->next) tail = tail->next;
        tail->next = deferred;
      }
    }
    const bool arrived = atexit_registrations != registrations_before;
    (void)pthread_mutex_unlock(&atexit_mutex);
    /* Settle when a whole pass produced no new registrations: any
     * entries left are for other DSO handles (already deferred) or
     * there are none. */
    if (!arrived) return;
  }
}

/* ---------------------------------------------------------------- */

namespace {

struct thread_atexit_record {
  void (*fn)(void*);
  void* arg;
};

void thread_atexit_adapter(void* p) {
  auto* record = static_cast<thread_atexit_record*>(p);
  record->fn(record->arg);
  free(record);
}

} /* namespace */

int __cxa_thread_atexit(void (*fn)(void*), void* arg, void* dso_handle) {
  (void)dso_handle; /* carried, ignored at thread exit by design */
  if (!fn) return -1;
  auto* record = static_cast<thread_atexit_record*>(
      malloc(sizeof(thread_atexit_record)));
  if (!record) return -1;
  record->fn = fn;
  record->arg = arg;
  if (lw_thread_atexit(&thread_atexit_adapter, record) != LITHOSYS_OK) {
    free(record);
    return -1;
  }
  return 0;
}

int __cxa_thread_atexit_impl(void (*fn)(void*), void* arg,
                             void* dso_handle) {
  return __cxa_thread_atexit(fn, arg, dso_handle);
}

} /* namespace __cxxabiv1 */
