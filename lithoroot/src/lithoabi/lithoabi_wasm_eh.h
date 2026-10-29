/* lithoabi_wasm_eh.h ... THE SEAM (internal).
 *
 * Every compiler- and target-specific exception-handling primitive
 * LithoAbi needs is declared here, and only here, with its semantics
 * specified in words. All portable logic (header layout, refcounts,
 * catch bookkeeping, the personality decision procedure, terminate
 * paths) is written against this interface in the other
 * src/lithoabi files and never touches a target intrinsic. The
 * toolchain piece reconciles exactly two implementation files
 * against this header:
 *
 *   src/lithoabi/backend_wasm.cc        the real target (wasm32)
 *   src/lithoabi/backend_native_scaffold.cc
 *                                       host-test scaffolding only
 *
 * If a spelling below turns out to differ from what stock clang
 * --target=wasm32 actually emits, the fix is confined to those
 * files; nothing else in LithoAbi changes.
 *
 * ==== The standing contract (purge charter, restated) ============
 *
 * The engine keeps WebAssembly exceptions. Exceptions thrown and
 * caught inside the module work. An exception that escapes the
 * module is fatal: no host catch machinery is promised, and the
 * host's current __cxa_throw stub (lkboot.js) already implements
 * exactly that fatal reading for the exceptions the in-module
 * runtime of the old build could not catch. LithoAbi moves
 * __cxa_throw in-module; the contract it must preserve is
 * caught-inside / uncaught-fatal.
 *
 * ==== Evidence from the current binary ==========================
 *
 * The Build 87c-era binary imports __cxa_throw with signature
 * (i32, i32, i32) -> () and contains no wasm exception tag (no tag
 * section, no tag import): the old toolchain split the runtime at
 * the throw, letting its in-module half do allocation, catch
 * bookkeeping and the personality work while the host performed the
 * non-returning act itself (a JS throw the page treats as fatal).
 * Stock clang with -fwasm-exceptions instead emits in-module throw
 * and catch instructions against a C++ exception TAG. Which of
 * these two shapes the toolchain piece lands on decides the
 * spellings in backend_wasm.cc:
 *
 *   Shape A (in-module wasm EH, expected): the tag is a wasm tag
 *   the module imports or defines under the name the compiler
 *   expects (candidate: "__cpp_exception"), and the throw primitive
 *   below is the wasm throw instruction applied to that tag with
 *   the exception object pointer as the payload.
 *
 *   Shape B (split at the throw, like today): the primitive is an
 *   imported function with exactly today's __cxa_throw signature
 *   shape, provided by the page layer, documented as non-returning,
 *   and in-module catch machinery must then be the legacy landing
 *   pad form the old compiler emitted. This shape cannot deliver
 *   real in-module catches of a host-thrown value through shared
 *   memory alone, which is why Shape A is the design center.
 *
 * ==== The primitives =============================================
 *
 * lithoabi_backend_throw(header):
 *   Begin unwinding for a fully initialized exception header (type,
 *   destructor and captured handlers are set; the caller has
 *   already charged the uncaught count). Never returns to its
 *   caller. Control reaches the landing pad of a handler in this
 *   module, which recovers the header through
 *   lithoabi_backend_caught_header and calls __cxa_begin_catch.
 *   If no handler in the module matches, the backend must end the
 *   process through lithoabi_backend_fatal_uncaught (below) rather
 *   than let a foreign value cross the module boundary: that is
 *   the uncaught-fatal half of the contract, enforced here because
 *   this is the last piece of our code that runs.
 *
 * lithoabi_backend_rethrow(header):
 *   Resume unwinding for the exception a handler is rethrowing. The
 *   caller (::__cxa_rethrow) has already unlinked the header from
 *   the caught chain and re-charged the uncaught count. Same
 *   delivery and same fatal rule as throw.
 *
 * lithoabi_backend_caught_header():
 *   Called by landing-pad glue (compiler-generated or the
 *   personality shim) at the point a catch has been entered:
 *   returns the header of the exception this landing pad caught.
 *   In Shape A this reads the value the wasm catch clause bound
 *   (the tag payload: the exception object pointer, from which the
 *   header is one fixed subtraction, see lithoabi_internal.h). In
 *   the native scaffolding it reads the in-flight slot the
 *   scaffolding's throw recorded. Either way the glue then calls
 *   __cxa_begin_catch(&header->unwindHeader).
 *
 * lithoabi_backend_fatal_uncaught(header):
 *   The uncaught-fatal sink: report through the console sink and
 *   end the instance (the portable side wires this to the
 *   terminate path; it is a backend function only because in
 *   Shape A the "no handler matched" discovery happens inside the
 *   unwinder, which the backend owns).
 *
 * ==== Personality support ========================================
 *
 * The personality decision procedure (personality.cc) is portable
 * LSDA byte-walking plus type matching. What it cannot do portably
 * is (1) find the LSDA for the frame it was invoked for, (2) turn
 * the LSDA's encoded type references into std::type_info pointers,
 * and (3) hand the chosen landing pad its switch value and adjusted
 * pointer. Those three are per-target and are specified here as an
 * opaque context with four accessors, implemented per backend:
 *
 *   lithoabi_personality_lsda(ctx, &lsda_base, &lsda_bytes):
 *     the LSDA byte range for the current frame.
 *   lithoabi_personality_ttype(ctx, lsda_base, encoded):
 *     resolve one encoded TType reference (already read out of the
 *     action record by the portable walker) to the type_info it
 *     denotes, or NULL for a catch-all entry (encoded value 0,
 *     which the walker passes through untouched).
 *   lithoabi_personality_install(ctx, switch_value, adjusted_ptr):
 *     record the outcome the landing pad will read: the action's
 *     switch value and the (possibly base-adjusted) exception
 *     object pointer from the winning __do_catch.
 *   lithoabi_personality_unwind_header(ctx):
 *     the _Unwind_Exception of the exception being unwound in this
 *     invocation, from which the portable side derives the header.
 *
 * In Shape A on wasm, ctx is whatever the compiler's landing pad
 * machinery carries (the lpad context the personality is invoked
 * with), lsda lookup is the compiler runtime's table walk, ttype
 * resolution follows the module's exception table encoding, and
 * install writes the lpad context's result slots. Those spellings
 * are the toolchain piece's to verify; the procedure they serve is
 * fixed by this header and personality.cc.
 */
#ifndef LITHOABI_WASM_EH_H
#define LITHOABI_WASM_EH_H

#include "lithoabi_internal.h"

namespace __cxxabiv1 {

/* ---- backend: throw delivery (implemented per target) ---------- */
[[noreturn]] void lithoabi_backend_throw(lithoabi_eh_header* header);
[[noreturn]] void lithoabi_backend_rethrow(lithoabi_eh_header* header);
lithoabi_eh_header* lithoabi_backend_caught_header();
[[noreturn]] void lithoabi_backend_fatal_uncaught(lithoabi_eh_header* header);

/* ---- backend: personality context (implemented per target) ----- */
struct lithoabi_personality_context;
const uint8_t* lithoabi_personality_lsda(lithoabi_personality_context* ctx,
                                         std::size_t* out_size);
const std::type_info* lithoabi_personality_ttype(
    lithoabi_personality_context* ctx, const uint8_t* lsda_base,
    uintptr_t encoded_ref);
void lithoabi_personality_install(lithoabi_personality_context* ctx,
                                  int switch_value, void* adjusted_ptr);
_Unwind_Exception* lithoabi_personality_unwind_header(
    lithoabi_personality_context* ctx);
/* The instruction pointer (as an offset into the LSDA's call-site
 * coordinate space) of the call whose frame is being unwound. In
 * the native scaffolding the tests set this directly. */
uintptr_t lithoabi_personality_ip(lithoabi_personality_context* ctx);

/* ---- the personality entry point (personality.cc) ---------------
 * Declared here rather than in cxxabi.h because its final
 * parameter is this header's opaque context: the portable decision
 * procedure behind it is complete, and what the toolchain piece
 * still owes is the compiler's real context type behind the
 * accessors above, plus confirmation of this entry's name and
 * linkage in the emitted code (in the DWARF model the personality
 * is a C-linkage symbol the unwinder calls by name; the wasm
 * model's invocation shape is the toolchain piece's to pin down,
 * in the same class as the throw tag's spelling). */
_Unwind_Reason_Code __gxx_personality_v0(int version,
                                         _Unwind_Action actions,
                                         uint64_t exception_class,
                                         _Unwind_Exception* unwind_exception,
                                         lithoabi_personality_context* ctx);

} /* namespace __cxxabiv1 */

#endif /* LITHOABI_WASM_EH_H */
