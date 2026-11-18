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
// Replaceable, for real (link wiring, 2026-11-16): the twenty
// replaceable forms are WEAK definitions, so a program that
// replaces them (LithoKraft's script_memory_hook.cpp does, to
// meter AngelScript's allocations) links without duplicate
// symbols and its strong definitions win everywhere, the way a
// hosted C++ library behaves. The two placement forms are not
// replaceable by the standard and stay strong.
//
// std::set_new_handler / std::get_new_handler now live in
// LithoAbi (src/lithoabi/std_exceptions.cc), next to the
// __cxa_new_handler storage they guard, so they exist in every
// link whether or not this file's operators are the ones used.
// The handler loop below reads that storage with an acquire load,
// pairing with the release exchange in set_new_handler.

#include <cstddef>
#include <stdlib.h>

#include <new>

#include "cxxabi.h"
#include "waferheap.h"

namespace __cxxabiv1 {
/* LithoAbi's bad_alloc thrower (src/lithoabi/eh_runtime.cc). */
[[noreturn]] void lithoabi_throw_bad_alloc();
} /* namespace __cxxabiv1 */

#define LITHOLIBC_REPLACEABLE __attribute__((weak))

namespace {

std::new_handler current_new_handler() {
  return __atomic_load_n(&__cxxabiv1::__cxa_new_handler, __ATOMIC_ACQUIRE);
}

void* allocate_or_handle(std::size_t size, bool throwing) {
  if (size == 0) size = 1;
  for (;;) {
    if (void* p = malloc(size)) return p;
    std::new_handler h = current_new_handler();
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
    std::new_handler h = current_new_handler();
    if (!h) {
      if (throwing) __cxxabiv1::lithoabi_throw_bad_alloc();
      return nullptr;
    }
    h();
  }
}

} /* namespace */

/* ---- the replaceable family ---------------------------------------------
 *
 * Only the two base forms allocate and free: operator new(size_t) and
 * operator delete(void*), plus their align_val_t twins. Every other
 * form reaches them the way [new.delete] specifies for the default
 * definitions (array new calls scalar new, nothrow new calls the
 * throwing form and catches, sized and array deletes call scalar
 * delete), so a program that replaces only the base forms sees every
 * allocation through its replacement, as it would with a hosted C++
 * library. All twenty are weak (see the note at the top). */

LITHOLIBC_REPLACEABLE void* operator new(std::size_t size) {
  return allocate_or_handle(size, true);
}
LITHOLIBC_REPLACEABLE void* operator new[](std::size_t size) {
  return ::operator new(size);
}
LITHOLIBC_REPLACEABLE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
LITHOLIBC_REPLACEABLE void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

/* ---- placement (completes LithoAbi's missing _ZnwmPv / _ZnamPv) -------- */

void* operator new(std::size_t, void* place) noexcept { return place; }
void* operator new[](std::size_t, void* place) noexcept { return place; }

/* ---- delete family ------------------------------------------------------- */

LITHOLIBC_REPLACEABLE void operator delete(void* p) noexcept { free(p); }
LITHOLIBC_REPLACEABLE void operator delete[](void* p) noexcept { ::operator delete(p); }
LITHOLIBC_REPLACEABLE void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
LITHOLIBC_REPLACEABLE void operator delete[](void* p, std::size_t) noexcept { ::operator delete[](p); }

/* ---- aligned (C++17) ------------------------------------------------------ */

LITHOLIBC_REPLACEABLE void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_aligned_or_handle(size, static_cast<std::size_t>(alignment),
                                    true);
}
LITHOLIBC_REPLACEABLE void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
LITHOLIBC_REPLACEABLE void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
LITHOLIBC_REPLACEABLE void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try {
    return ::operator new[](size, alignment);
  } catch (...) {
    return nullptr;
  }
}
LITHOLIBC_REPLACEABLE void operator delete(void* p, std::align_val_t) noexcept { free(p); }
LITHOLIBC_REPLACEABLE void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
LITHOLIBC_REPLACEABLE void operator delete(void* p, std::size_t, std::align_val_t a) noexcept {
  ::operator delete(p, a);
}
LITHOLIBC_REPLACEABLE void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept {
  ::operator delete[](p, a);
}

/* S2: the nothrow delete forms <new> now declares (called when a
 * nothrow-new'd object's constructor throws). */
LITHOLIBC_REPLACEABLE void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
LITHOLIBC_REPLACEABLE void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }
LITHOLIBC_REPLACEABLE void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p, a); }
LITHOLIBC_REPLACEABLE void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete[](p, a); }

/* ---- the nothrow object and the handler accessors -------------------------
 *
 * std::nothrow is owned by LithoAbi's std_exceptions.cc (it travels
 * with the exception classes in <new>); this file only consumes the
 * extern declaration. set_new_handler / get_new_handler moved there
 * too (see the note at the top). */
