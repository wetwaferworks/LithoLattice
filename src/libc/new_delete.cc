// new_delete.cc ... the operator new / delete family, completing
// the debt LithoAbi's note left to this piece (docs/LITHOABI.md:
// _Znwm, _ZdlPv, the sized and aligned variants, referenced by
// LithoAbi's own translation units but undefined there).
//
// Everything allocates through LithoLibC malloc (WaferHeap
// underneath): plain forms over malloc, aligned forms over
// aligned_alloc, delete forms over free (which is alignment
// agnostic in WaferHeap, so sized and aligned deletes are plain
// frees; the size argument of a sized delete is ignored, as the
// contract allows).
//
// Failure discipline: a throwing form runs the standard
// new-handler loop over the handler LithoAbi stores
// (abi::__cxa_new_handler, declared in <cxxabi.h>) and, with no
// handler installed, throws std::bad_alloc through LithoAbi's
// designated entry point, which is the one consumer allowed to
// draw on the exception emergency pool's reserved bad_alloc
// slot. The entry point is declared locally below rather than
// through LithoAbi's internal header: it is a two-library
// contract (LithoAbi provides, LithoLibC consumes), and pulling
// the whole internal header across the boundary for one function
// would couple more than the contract says.
//
// nothrow forms run the same handler loop and return nullptr
// when no handler is installed.
//
// Definitions shipped here whose DECLARATIONS are still owed by
// the seeded <new> header (which this piece may not edit, and
// which says the stdlib piece widens it): std::set_new_handler
// and std::get_new_handler, and the operator declarations
// themselves. The definitions below carry the exact standard
// signatures, so the widening is a declaration-only change when
// it happens. Flagged in docs/LITHOLIBC-STDIO.md as header debt.

#include <cstddef>
#include <stdlib.h>

#include <new>

#include "cxxabi.h"
#include "waferheap.h"

namespace __cxxabiv1 {
/* LithoAbi's bad_alloc thrower (src/lithoabi/eh_runtime.cc). */
[[noreturn]] void lithoabi_throw_bad_alloc();
} /* namespace __cxxabiv1 */

namespace {

void* allocate_or_handle(std::size_t size, bool throwing) {
  if (size == 0) size = 1;
  for (;;) {
    if (void* p = malloc(size)) return p;
    std::new_handler h = __cxxabiv1::__cxa_new_handler;
    if (!h) {
      if (throwing) __cxxabiv1::lithoabi_throw_bad_alloc();
      return nullptr;
    }
    h();
  }
}

void* allocate_aligned_or_handle(std::size_t size, std::size_t alignment,
                                 bool throwing) {
  if (size == 0) size = 1;
  if (alignment <= WH_ALIGNMENT) {
    return allocate_or_handle(size, throwing);
  }
  for (;;) {
    if (void* p = aligned_alloc(alignment, size)) return p;
    std::new_handler h = __cxxabiv1::__cxa_new_handler;
    if (!h) {
      if (throwing) __cxxabiv1::lithoabi_throw_bad_alloc();
      return nullptr;
    }
    h();
  }
}

} /* namespace */

/* ---- scalar and array -------------------------------------------------- */

void* operator new(std::size_t size) {
  return allocate_or_handle(size, true);
}
void* operator new[](std::size_t size) {
  return allocate_or_handle(size, true);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return allocate_or_handle(size, false);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return allocate_or_handle(size, false);
}

/* ---- placement (completes LithoAbi's missing _ZnwmPv / _ZnamPv) -------- */

void* operator new(std::size_t, void* place) noexcept { return place; }
void* operator new[](std::size_t, void* place) noexcept { return place; }

/* ---- delete family ------------------------------------------------------- */

void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, std::size_t) noexcept { free(p); }
void operator delete[](void* p, std::size_t) noexcept { free(p); }

/* ---- aligned (C++17) ------------------------------------------------------ */

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_or_handle(size, static_cast<std::size_t>(alignment),
                                    true);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_or_handle(size, static_cast<std::size_t>(alignment),
                                    true);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return allocate_aligned_or_handle(size, static_cast<std::size_t>(alignment),
                                    false);
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return allocate_aligned_or_handle(size, static_cast<std::size_t>(alignment),
                                    false);
}
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  free(p);
}

/* ---- the nothrow object and the handler accessors ------------------------- */

namespace std {

/* std::nothrow itself is NOT defined here: LithoAbi's
 * std_exceptions.cc owns that object (it travels with the
 * exception classes in <new>). Defining it in both places
 * collided at the first joint link of the two pieces; this
 * file only consumes the extern declaration. */

new_handler set_new_handler(new_handler h) noexcept {
  new_handler old = __cxxabiv1::__cxa_new_handler;
  __cxxabiv1::__cxa_new_handler = h;
  return old;
}

new_handler get_new_handler() noexcept {
  return __cxxabiv1::__cxa_new_handler;
}

} /* namespace std */
