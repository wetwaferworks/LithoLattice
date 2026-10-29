/* eh_runtime.cc ... the throw / catch bookkeeping core:
 * __cxa_throw, __cxa_rethrow, __cxa_begin_catch, __cxa_end_catch,
 * __cxa_get_exception_ptr, __cxa_current_exception_type,
 * __cxa_rethrow_primary_exception, the terminate / unexpected
 * entry points a landing pad can call, and the compiler's own
 * bad_cast / bad_typeid throws.
 *
 * The caught chain lives in the per-thread eh globals:
 * caughtExceptions is the innermost currently HANDLED exception.
 * begin_catch pushes, end_catch pops the innermost, rethrow pops
 * and hands the exception back to the backend. The backend (see
 * lithoabi_wasm_eh.h) performs the actual unwind; everything about
 * WHAT is caught, in what state, and when it dies is decided here,
 * portably.
 */

#include <typeinfo>

#include "lithoabi_internal.h"
#include "lithoabi_wasm_eh.h"

/* Placement new, declared for this translation unit only. The
 * runtime constructs its own exception objects in storage the
 * allocation layer already produced; that needs the placement form
 * and nothing else from the operator new family. The allocating
 * forms are the stdlib piece's to declare in <new>; a declaration
 * here (no definition: the compiler supplies the standard
 * placement semantics, return the given pointer) keeps this file
 * honest without widening the seeded header. */
void* operator new(std::size_t, void*) noexcept;

namespace __cxxabiv1 {

static lithoabi_eh_header* lithoabi_globals_caught_head(
    __cxa_eh_globals* globals) {
  return reinterpret_cast<lithoabi_eh_header*>(globals->caughtExceptions);
}
static void lithoabi_globals_set_caught_head(__cxa_eh_globals* globals,
                                             lithoabi_eh_header* header) {
  globals->caughtExceptions =
      reinterpret_cast<__cxa_exception*>(header);
}

/* ---------------------------------------------------------------- */

[[noreturn]] void __cxa_throw(void* thrown_exception, std::type_info* tinfo,
                              __cxa_destructor_fn dest) {
  __cxa_eh_globals* globals = __cxa_get_globals();
  lithoabi_eh_header* header = lithoabi_header_of_object(thrown_exception);

  header->exceptionType = tinfo;
  header->exceptionDestructor = dest;
  header->unexpectedHandler = __cxa_unexpected_handler;
  header->terminateHandler = __cxa_terminate_handler;
  header->nextException = nullptr;
  header->handlerCount = 0;
  header->handlerSwitchValue = 0;
  header->actionRecord = nullptr;
  header->languageSpecificData = nullptr;
  header->catchTemp = nullptr;
  header->adjustedPtr = thrown_exception;

  globals->uncaughtExceptions += 1;
  lithoabi_backend_throw(header);
  /* The backend never returns (handler or fatal); this is reached
   * only if a backend is mislinked. */
  lithoabi_terminate_now("lithoabi: throw backend returned\n");
}

[[noreturn]] void __cxa_rethrow() {
  __cxa_eh_globals* globals = __cxa_get_globals();
  lithoabi_eh_header* header = lithoabi_globals_caught_head(globals);
  if (!header) {
    lithoabi_terminate_now(
        "lithoabi: rethrow with no exception being handled\n");
  }
  /* Unlink eagerly, and record the pairing token: the delivery
   * model's pads run __cxa_end_catch once as the rethrown
   * exception leaves this handler (its cleanup pad), and that
   * call must stand down (see __cxa_end_catch) rather than pop an
   * outer exception or find an empty chain. libc++abi solves the
   * same pairing by negating handlerCount and unlinking inside
   * end_catch; the token reaches the same observable state while
   * keeping this library's invariant that the chain only ever
   * holds exceptions a live handler owns. The toolchain piece's
   * probe convicted both naive forms (pop with no token: the
   * pad's end_catch terminates on the empty chain; negate with no
   * scaffold pairing: the next begin_catch self-links). */
  lithoabi_globals_set_caught_head(globals, header->nextException);
  header->nextException = nullptr;
  header->handlerCount -= 1;
  globals->lithoabiRethrownPending =
      reinterpret_cast<__cxa_exception*>(header);
  globals->uncaughtExceptions += 1;
  lithoabi_backend_rethrow(header);
  lithoabi_terminate_now("lithoabi: rethrow backend returned\n");
}

void* __cxa_begin_catch(void* exc_obj_in) {
  auto* ue = static_cast<_Unwind_Exception*>(exc_obj_in);
  lithoabi_eh_header* header = lithoabi_header_of_unwind(ue);
  __cxa_eh_globals* globals = __cxa_get_globals();
  /* A fresh catch supersedes any pending rethrow pairing: the
   * rethrown exception has arrived somewhere, and the end_catch
   * that eventually runs belongs to THIS handler. */
  globals->lithoabiRethrownPending = nullptr;

  /* The personality (or the landing pad, in the legacy delivery)
   * has already matched this exception and stored the adjusted
   * pointer; if nobody did, the object itself is the adjustment. */
  if (!header->adjustedPtr) {
    header->adjustedPtr = lithoabi_object_of_header(header);
  }
  header->handlerCount += 1;
  if (globals->uncaughtExceptions > 0) globals->uncaughtExceptions -= 1;
  header->nextException = lithoabi_globals_caught_head(globals);
  lithoabi_globals_set_caught_head(globals, header);
  return header->adjustedPtr;
}

void* __cxa_get_exception_ptr(void* exc_obj_in) {
  auto* ue = static_cast<_Unwind_Exception*>(exc_obj_in);
  lithoabi_eh_header* header = lithoabi_header_of_unwind(ue);
  return header->adjustedPtr ? header->adjustedPtr
                             : lithoabi_object_of_header(header);
}

void __cxa_end_catch() {
  __cxa_eh_globals* globals = __cxa_get_globals();
  if (globals->lithoabiRethrownPending != nullptr) {
    /* This call pairs with a rethrow, not with a live handler:
     * __cxa_rethrow already unlinked and accounted the exception,
     * and this is the handler-exit cleanup call the delivery
     * model emits as the rethrown exception leaves. Stand down. */
    globals->lithoabiRethrownPending = nullptr;
    return;
  }
  lithoabi_eh_header* header = lithoabi_globals_caught_head(globals);
  if (!header) {
    lithoabi_terminate_now(
        "lithoabi: end_catch with no exception being handled\n");
  }
  lithoabi_globals_set_caught_head(globals, header->nextException);
  header->nextException = nullptr;
  header->handlerCount -= 1;
  /* Releasing the in-flight ownership the handler borrowed: if this
   * was the last owner and no other handler holds the exception,
   * it dies here. */
  la_atomic_fetch_sub32(&header->referenceCount, 1);
  (void)lithoabi_maybe_release(header);
}

std::type_info* __cxa_current_exception_type() {
  __cxa_eh_globals* globals = __cxa_get_globals_fast();
  if (!globals) return nullptr;
  lithoabi_eh_header* header = lithoabi_globals_caught_head(globals);
  return header ? header->exceptionType : nullptr;
}

void __cxa_rethrow_primary_exception(
    __cxa_exception* primary) {
  if (!primary) {
    lithoabi_terminate_now(
        "lithoabi: rethrow of an empty primary exception\n");
  }
  auto* header = reinterpret_cast<lithoabi_eh_header*>(primary);
  __cxa_eh_globals* globals = __cxa_get_globals();
  /* Audit fix D-F1: the primary may be the exception this thread
   * is currently handling (the exception_ptr idiom: capture with
   * std::current_exception inside a handler and rethrow it from
   * the same handler). Then the header is linked into this
   * thread's caught chain, and handing it to the backend as-is
   * lets the next begin_catch link the header to itself and
   * strand the chain head on a dead handler.
   *
   * Toolchain-piece amendment (2026-10-28): the HEAD case also
   * records the rethrow pairing token (see __cxa_rethrow), so
   * the handler-exit end_catch the delivery model emits stands
   * down instead of popping the next exception. A primary caught
   * DEEPER than the head records no token: the innermost
   * handler's end_catch still has its own head to account. */
  {
    lithoabi_eh_header* head = lithoabi_globals_caught_head(globals);
    if (head == header) {
      lithoabi_globals_set_caught_head(globals, header->nextException);
      header->nextException = nullptr;
      header->handlerCount -= 1;
      globals->lithoabiRethrownPending =
          reinterpret_cast<__cxa_exception*>(header);
    } else if (head != nullptr) {
      lithoabi_eh_header* cur = head;
      while (cur->nextException != nullptr &&
             cur->nextException != header) {
        cur = cur->nextException;
      }
      if (cur->nextException == header) {
        cur->nextException = header->nextException;
        header->nextException = nullptr;
        header->handlerCount -= 1;
      }
    }
  }
  /* The stdlib's exception_ptr releases its ownership as part of
   * the rethrow call (that is the documented contract of this
   * entry point), so the in-flight state takes over the count the
   * pointer held; the reference count is therefore not touched
   * here, and the count the final end_catch releases is unchanged.
   * For a primary at rest handlerCount is 0 and the unlink above
   * is a no-op; the exception becomes the in-flight one and
   * end-of-catch or a further capture disposes of it through the
   * normal rules. */
  header->unexpectedHandler = __cxa_unexpected_handler;
  header->terminateHandler = __cxa_terminate_handler;
  globals->uncaughtExceptions += 1;
  lithoabi_backend_rethrow(header);
  lithoabi_terminate_now("lithoabi: rethrow backend returned\n");
}

/* ----------------------------------------------------------------
 * Landing-pad terminate / unexpected entries.
 */

[[noreturn]] void __cxa_call_terminate(void* exc_obj_in) {
  (void)__cxa_begin_catch(exc_obj_in);
  std::terminate();
}

[[noreturn]] void __clang_call_terminate(void* exc_obj_in) {
  (void)__cxa_begin_catch(exc_obj_in);
  std::terminate();
}

[[noreturn]] void __cxa_call_unexpected(void* exc_obj_in) {
  auto* ue = static_cast<_Unwind_Exception*>(exc_obj_in);
  lithoabi_eh_header* header = lithoabi_header_of_unwind(ue);
  std::unexpected_handler handler = header->unexpectedHandler;
  if (handler) handler();
  /* The handler returned. Per the ABI the only remaining legal
   * outcomes run through bad_exception against the violated
   * specification, which needs the action record's filter data;
   * dynamic exception specifications are removed from the language
   * and no code the toolchain compiles from the engine can produce
   * one, so the honest end here is terminate. If the handler
   * THREW, control never reaches this line: the new exception is
   * already in flight through the backend, which is the documented
   * simplification in docs/LITHOABI.md. */
  std::terminate();
}

/* ----------------------------------------------------------------
 * The runtime's own typed throws.
 */

/* The destructor handed to __cxa_throw for the runtime's own
 * exceptions, spelled in the target's destructor function type
 * (see __cxa_destructor_fn in cxxabi.h). */
template <typename E>
__cxa_destructor_fn lithoabi_own_dest() {
#ifdef __wasm__
  return [](void* p) -> void* {
    static_cast<E*>(p)->~E();
    return p;
  };
#else
  return [](void* p) { static_cast<E*>(p)->~E(); };
#endif
}

template <typename E>
[[noreturn]] void lithoabi_throw_own(bool for_bad_alloc) {
  void* object = for_bad_alloc
                     ? lithoabi_allocate_exception_for_bad_alloc(sizeof(E))
                     : __cxa_allocate_exception(sizeof(E));
  new (object) E();
  __cxa_throw(object, const_cast<std::type_info*>(&typeid(E)),
              lithoabi_own_dest<E>());
}

[[noreturn]] void __cxa_bad_cast() {
  lithoabi_throw_own<std::bad_cast>(false);
}

[[noreturn]] void __cxa_bad_typeid() {
  lithoabi_throw_own<std::bad_typeid>(false);
}

/* The runtime's own bad_alloc thrower: the entry the stdlib
 * piece's operator new failure path calls, so the emergency pool's
 * reserved slot has exactly one disciplined consumer. */
[[noreturn]] void lithoabi_throw_bad_alloc() {
  lithoabi_throw_own<std::bad_alloc>(true);
}

} /* namespace __cxxabiv1 */
