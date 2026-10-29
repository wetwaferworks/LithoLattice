/* backend_native_scaffold.cc ... TEST-ONLY EH backend.
 *
 * The native scaffolding implementation of the seam in
 * lithoabi_wasm_eh.h, existing so the portable LithoAbi logic can
 * be exercised on the host. It models the wasm EH delivery shape:
 *
 *   throw:   the portable side has fully built the exception
 *            header; the backend records it in the per-thread
 *            in-flight slot and jumps to the innermost pushed
 *            catch frame (longjmp), popping that frame as it goes
 *            (a caught exception's frame is consumed).
 *   land:    the test's landing-pad arm reads the header back
 *            through lithoabi_backend_caught_header (or the
 *            scaffold's take_caught), performs the personality's
 *            matching job by hand with __do_catch, and calls
 *            __cxa_begin_catch, exactly as compiler glue would.
 *   rethrow: same delivery as throw; because the catching frame
 *            was popped on landing, delivery reaches the next
 *            outer frame.
 *   escape:  a throw with no frame on the stack is the
 *            uncaught-fatal half of the contract: report and run
 *            the terminate path, which ends in lithosys_abort.
 *
 * This file is scaffolding. It proves the portable logic's
 * behavior under a faithful model of delivery; it does not prove
 * the wasm throw itself (docs/LITHOABI.md is explicit about the
 * boundary). It is never linked into a production build.
 */

#include <cstddef>
#include <cstdint>

#include "lithoabi_internal.h"
#include "lithoabi_wasm_eh.h"
#include "lithoabi_test_scaffold.h"

namespace __cxxabiv1 {
namespace {

constexpr int kMaxFrames = 64;

__thread lithoabi_scaffold_frame* g_frames[kMaxFrames];
__thread int g_frame_depth = 0;
__thread lithoabi_eh_header* g_in_flight = nullptr;
__thread int g_throw_count = 0;

[[noreturn]] void deliver(lithoabi_eh_header* header) {
  g_in_flight = header;
  g_throw_count += 1;
  if (g_frame_depth <= 0) {
    lithoabi_backend_fatal_uncaught(header);
  }
  lithoabi_scaffold_frame* frame = g_frames[--g_frame_depth];
  longjmp(frame->env, 1);
}

} /* namespace */

void lithoabi_scaffold_push_frame(lithoabi_scaffold_frame* frame) {
  if (g_frame_depth >= kMaxFrames) {
    lithoabi_terminate_now("lithoabi scaffold: catch frame overflow");
  }
  g_frames[g_frame_depth++] = frame;
}

void lithoabi_scaffold_pop_frame() {
  if (g_frame_depth > 0) g_frame_depth -= 1;
}

int lithoabi_scaffold_frame_depth() { return g_frame_depth; }

lithoabi_eh_header* lithoabi_scaffold_take_caught() {
  lithoabi_eh_header* header = g_in_flight;
  g_in_flight = nullptr;
  return header;
}

int lithoabi_scaffold_throw_count() { return g_throw_count; }

/* ---- the seam: throw delivery ---- */

[[noreturn]] void lithoabi_backend_throw(lithoabi_eh_header* header) {
  deliver(header);
}

[[noreturn]] void lithoabi_backend_rethrow(lithoabi_eh_header* header) {
  deliver(header);
}

lithoabi_eh_header* lithoabi_backend_caught_header() { return g_in_flight; }

[[noreturn]] void lithoabi_backend_fatal_uncaught(
    lithoabi_eh_header* header) {
  const char* name = "<unknown>";
  if (header != nullptr && header->exceptionType != nullptr &&
      header->exceptionType->name() != nullptr) {
    name = header->exceptionType->name();
  }
  lithoabi_diag("lithoabi: uncaught exception escaped (type ", name,
                ") ... fatal per the caught-inside contract\n");
  lithoabi_terminate_now("uncaught exception");
}

/* ---- the seam: personality context ---- */

const uint8_t* lithoabi_personality_lsda(lithoabi_personality_context* ctx,
                                         std::size_t* out_size) {
  if (ctx == nullptr) return nullptr;
  if (out_size != nullptr) *out_size = ctx->lsda_size;
  return ctx->lsda;
}

const std::type_info* lithoabi_personality_ttype(
    lithoabi_personality_context* ctx, const uint8_t*, uintptr_t encoded_ref) {
  if (ctx == nullptr || encoded_ref == 0) return nullptr;
  if (encoded_ref > ctx->ttype_count) return nullptr;
  return ctx->ttype_table[encoded_ref - 1];
}

void lithoabi_personality_install(lithoabi_personality_context* ctx,
                                  int switch_value, void* adjusted_ptr) {
  if (ctx == nullptr) return;
  ctx->install_calls += 1;
  ctx->installed_switch = switch_value;
  ctx->installed_adjusted = adjusted_ptr;
}

_Unwind_Exception* lithoabi_personality_unwind_header(
    lithoabi_personality_context* ctx) {
  return ctx == nullptr ? nullptr : ctx->unwind;
}

uintptr_t lithoabi_personality_ip(lithoabi_personality_context* ctx) {
  return ctx == nullptr ? 0 : ctx->ip;
}

} /* namespace __cxxabiv1 */
