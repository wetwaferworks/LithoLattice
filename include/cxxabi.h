/* cxxabi.h ... LithoAbi, the C++ runtime glue of LithoRoot.
 *
 * This header declares the Itanium C++ ABI entry points LithoAbi
 * provides in-module: the exception allocation / throw / catch
 * family, the primary-exception (exception_ptr) primitives, the
 * guard family for function-local statics, the atexit family, the
 * pure / deleted virtual stubs, dynamic_cast, and the handler
 * variables. It is the surface the toolchain piece will compile the
 * engine's C++ against once the vendor runtime is gone.
 *
 * The surface was derived from the current engine binary's census
 * (docs/LITHOABI.md has the full derivation): the binary imports
 * exactly one ABI function (__cxa_throw) and defines the rest of the
 * vendor ABI library invisibly inside itself, so the set below is
 * (a) every ABI entry point the engine's compiled code can be shown
 * or expected to reference, plus (b) the principled minimum the
 * compiler emits for the C++ the engine demonstrably uses (class
 * catches with base adjustment, std::exception_ptr across threads,
 * dynamic_cast, function-local statics). Anything deliberately not
 * provided (dependent exceptions, the __cxa_vec_* array helpers,
 * the demangler) is listed with its reason in docs/LITHOABI.md.
 *
 * std::type_info appears throughout by pointer only; its layout is
 * declared in <typeinfo>. The __cxxabiv1 type_info classes are
 * internal (src/lithoabi/typeinfo_layout.h); only forward
 * declarations cross this header.
 */
#ifndef LITHOABI_CXXABI_H
#define LITHOABI_CXXABI_H

#include <cstddef>
#include <exception>
#include <new>
#include <typeinfo>

namespace __cxxabiv1 {

class __class_type_info;

/* Opaque in the public header; the layout is LithoAbi-internal
 * (src/lithoabi/lithoabi_internal.h). */
struct __cxa_exception;

struct __cxa_eh_globals {
  __cxa_exception* caughtExceptions;
  unsigned int uncaughtExceptions;
  /* LithoAbi extension (not an Itanium field): the header most
   * recently rethrown on this thread whose paired __cxa_end_catch
   * has not yet run. Rethrow unlinks eagerly (see __cxa_rethrow),
   * and the delivery model's handler-exit cleanup pad still calls
   * __cxa_end_catch once for the exited handler; this token lets
   * that call recognize its pairing and stand down instead of
   * popping an outer exception. __cxa_begin_catch clears it: a
   * fresh catch supersedes any pending pairing. Consumers treat
   * this struct as opaque; only __cxa_get_globals allocates it. */
  __cxa_exception* lithoabiRethrownPending;
};

extern "C" {

/* ---- exception allocation --------------------------------------
 * __cxa_allocate_exception returns space for an exception object of
 * thrown_size bytes, preceded by LithoAbi's header. It never returns
 * NULL: malloc first, then the static emergency pool, then the
 * terminate path (see docs/LITHOABI.md for the pool discipline,
 * including the slot reserved for bad_alloc). __cxa_free_exception
 * releases a pointer from __cxa_allocate_exception whose object was
 * never thrown (or has been fully released). */
void* __cxa_allocate_exception(std::size_t thrown_size);
void __cxa_free_exception(void* thrown_exception);

/* Build a primary exception around an object the caller allocated
 * (with __cxa_allocate_exception) and constructed: allocates the
 * refcounted header, wires type and destructor, returns the opaque
 * header handle with reference count 1. This plus the four functions
 * below are the whole ABI-side contract of std::exception_ptr. */
/* The destructor function type the throw machinery carries. The
 * WebAssembly C++ ABI returns `this` from destructors, so under
 * __wasm__ the type is void* (*)(void*); everywhere else it is the
 * classic Itanium void (*)(void*). libc++abi spells the same split
 * (its _LIBCXXABI_DTOR_FUNC, with the note "In Wasm, a destructor
 * returns its argument"). Keyed here, once, so the header layout
 * and every signature agree per target. Settled by the Stage 2
 * toolchain piece: stock clang emits D2 with the this-returning
 * type, and a (void)(void*) call_indirect over it traps. */
#ifdef __wasm__
using __cxa_destructor_fn = void* (*)(void*);
#else
using __cxa_destructor_fn = void (*)(void*);
#endif

__cxa_exception* __cxa_init_primary_exception(void* object,
                                              std::type_info* rtti,
                                              __cxa_destructor_fn destructor);

/* ---- throw / catch ---------------------------------------------
 * __cxa_throw never returns: control reaches an in-module handler,
 * or the process dies on the uncaught-fatal path (the standing
 * contract: caught-inside works, escaping the module is fatal).
 * __cxa_begin_catch takes the unwind exception pointer the landing
 * pad was handed (in-module: the address of the header's unwind
 * member; the seam header specifies how each target delivers it)
 * and returns nothing; __cxa_get_exception_ptr then yields the
 * adjusted object pointer the personality (or the landing pad, in
 * the legacy delivery) selected. __cxa_end_catch operates on the
 * innermost currently handled exception. */
[[noreturn]] void __cxa_throw(void* thrown_exception,
                              std::type_info* tinfo,
                              __cxa_destructor_fn dest);
[[noreturn]] void __cxa_rethrow();
void* __cxa_begin_catch(void* exc_obj_in);
void __cxa_end_catch();
void* __cxa_get_exception_ptr(void* exc_obj_in);
std::type_info* __cxa_current_exception_type();

/* ---- primary exceptions (std::exception_ptr support) ------------ */
__cxa_eh_globals* __cxa_get_globals();
__cxa_eh_globals* __cxa_get_globals_fast();
__cxa_exception* __cxa_current_primary_exception();
void __cxa_rethrow_primary_exception(__cxa_exception* primary);
void __cxa_increment_exception_refcount(__cxa_exception* primary);
void __cxa_decrement_exception_refcount(__cxa_exception* primary);

/* ---- terminate / unexpected -------------------------------------
 * __cxa_call_terminate is entered from a landing pad when an
 * exception escapes a noexcept function or a cleanup throws; it
 * performs a begin-catch on the argument and calls the current
 * terminate handler. __clang_call_terminate is the entry clang
 * actually emits in noexcept landing pads; same contract, provided
 * under both names. __cxa_call_unexpected serves violated dynamic
 * exception specifications: the exception header carries the
 * unexpected handler captured at throw time, and this calls it. */
[[noreturn]] void __cxa_call_terminate(void* exc_obj_in);
[[noreturn]] void __cxa_call_unexpected(void* exc_obj_in);
[[noreturn]] void __clang_call_terminate(void* exc_obj_in);

/* Thrown by the compiler's own code paths: dynamic_cast on a null
 * polymorphic reference, typeid on a null polymorphic glvalue. */
[[noreturn]] void __cxa_bad_cast();
[[noreturn]] void __cxa_bad_typeid();

/* ---- guards (function-local statics) ----------------------------
 * The guard object is 8 bytes in the emitted code's static storage;
 * its first byte is the completion flag the compiler's fast path
 * reads. acquire returns nonzero exactly once per guard (the caller
 * then runs the initializer and calls release, or abort if the
 * initializer throws). Thread-safe over LithoThread. */
int __cxa_guard_acquire(long long* guard);
void __cxa_guard_release(long long* guard);
void __cxa_guard_abort(long long* guard);

/* ---- exit registration ------------------------------------------
 * __cxa_atexit registers fn(arg) to run at process exit (or at the
 * finalize of its DSO handle), in the pass discipline documented in
 * docs/LITHOABI.md (reverse registration order within a pass,
 * registrations made during a pass deferred to the next, passes
 * capped). Returns 0 on success, nonzero on allocation failure.
 * __cxa_finalize runs the registrations for one DSO handle, or all
 * of them when passed NULL. __dso_handle is the module's own handle:
 * its address is the token the compiler passes for main-module
 * registrations. */
int __cxa_atexit(void (*fn)(void*), void* arg, void* dso_handle);
void __cxa_finalize(void* dso_handle);

/* Thread exit registration: fn(arg) runs when the calling thread
 * exits. Rides LithoWeave's destructor registry (Piece 4); the DSO
 * handle is carried for symmetry and ignored at thread exit, which
 * matches the reference behavior (thread destructors all run
 * regardless of DSO). __cxa_thread_atexit_impl is the alternate
 * spelling some targets emit; identical function. */
int __cxa_thread_atexit(void (*fn)(void*), void* arg, void* dso_handle);
int __cxa_thread_atexit_impl(void (*fn)(void*), void* arg, void* dso_handle);

/* ---- misc stubs --------------------------------------------------
 * Both print a diagnostic through lithosys_console_write and end in
 * the terminate path. */
[[noreturn]] void __cxa_pure_virtual();
[[noreturn]] void __cxa_deleted_virtual();

/* ---- dynamic cast ------------------------------------------------
 * The full Itanium search: src_type and dst_type are the static and
 * dynamic class typeinfos, src_ptr the pointer being cast, and
 * src2dst the offset hint emitted by the compiler (0 for a whole
 * object, -1 for "unknown, use the vtable's dynamic type", -2 for
 * the cross-cast probe form). Returns the adjusted pointer or NULL. */
void* __dynamic_cast(const void* src_ptr,
                     const __class_type_info* src_type,
                     const __class_type_info* dst_type,
                     std::ptrdiff_t src2dst);

/* ---- handler variables -------------------------------------------
 * Owned by LithoAbi, assigned by std::set_terminate and friends.
 * Defaults: terminate writes a diagnostic and aborts through
 * LithoSys; unexpected calls terminate; the new handler starts
 * empty (NULL), per the standard's initial state. */
extern std::terminate_handler __cxa_terminate_handler;
extern std::unexpected_handler __cxa_unexpected_handler;
extern std::new_handler __cxa_new_handler;

} /* extern "C" */

} /* namespace __cxxabiv1 */

namespace abi = __cxxabiv1;

/* The module's own DSO handle lives at global scope, where the
 * compiler's emitted registrations look for it: its address is the
 * token passed as the third argument of __cxa_atexit for
 * main-module destructors. Defined in src/lithoabi/atexit.cc. */
extern "C" {
extern void* __dso_handle;
}

#endif /* LITHOABI_CXXABI_H */
