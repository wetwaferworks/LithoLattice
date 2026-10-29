/* lithoabi_test_scaffold.h ... declarations for the native test
 * scaffolding backend (backend_native_scaffold.cc).
 *
 * This header is TEST INFRASTRUCTURE, not part of LithoAbi's
 * production surface: it gives the host test the concrete shape of
 * the seam's opaque personality context (which the native backend
 * implements with plain fields the test sets directly) and the
 * catch-frame discipline the scaffolding models wasm EH with.
 * Nothing under src/lithoabi except the native backend includes
 * this file, and the wasm backend never sees it.
 */
#ifndef LITHOABI_TEST_SCAFFOLD_H
#define LITHOABI_TEST_SCAFFOLD_H

#include <setjmp.h>
#include <cstddef>
#include <cstdint>
#include <typeinfo>

#include "lithoabi_internal.h"
#include "lithoabi_wasm_eh.h"

namespace __cxxabiv1 {

/* The native personality context: plain fields, set by the test.
 * ttype_table maps the synthetic LSDA's raw TType references
 * (1-based) to type_info pointers; install captures what the
 * personality procedure decided so the test can assert on it. */
struct lithoabi_personality_context {
  const uint8_t* lsda = nullptr;
  std::size_t lsda_size = 0;
  uintptr_t ip = 0;
  _Unwind_Exception* unwind = nullptr;
  const std::type_info* const* ttype_table = nullptr;
  std::size_t ttype_count = 0;
  int install_calls = 0;
  int installed_switch = -1;
  void* installed_adjusted = nullptr;
};

/* Catch frames. The discipline models the wasm catch clause with
 * setjmp/longjmp:
 *
 *   lithoabi_scaffold_frame frame;
 *   lithoabi_scaffold_push_frame(&frame);
 *   if (LITHOABI_SCAFFOLD_TRY(&frame)) {
 *     ... try body; __cxa_throw lands in the else arm ...
 *     lithoabi_scaffold_pop_frame();   // normal completion only
 *   } else {
 *     // landing pad: the backend already popped this frame
 *     lithoabi_eh_header* h = lithoabi_scaffold_take_caught();
 *     ...
 *   }
 *
 * A rethrow from inside a handler therefore delivers to the next
 * outer frame, exactly as unwinding resumes past the catching
 * frame. Frames and the in-flight slot are per-thread. */
struct lithoabi_scaffold_frame {
  jmp_buf env;
};

void lithoabi_scaffold_push_frame(lithoabi_scaffold_frame* frame);
void lithoabi_scaffold_pop_frame();
int lithoabi_scaffold_frame_depth();
/* The header the current landing pad caught; clears the slot. */
lithoabi_eh_header* lithoabi_scaffold_take_caught();
/* How many throws/rethrows this thread has delivered. */
int lithoabi_scaffold_throw_count();

} /* namespace __cxxabiv1 */

/* setjmp must expand in the caller's own function: a macro, not a
 * function, by necessity. Nonzero return means an exception landed
 * (the landing-pad arm). */
#define LITHOABI_SCAFFOLD_TRY(frame) (setjmp((frame)->env) == 0)

#endif /* LITHOABI_TEST_SCAFFOLD_H */
