// LithoAbi: wasm back-end (compiled ONLY for the wasm build).
//
// RECONCILED by the Stage 2 toolchain piece (2026-10-28) against
// the exact emission model of stock clang 20.1.8 with
// -fwasm-exceptions. The model, all of it verified from probe
// objects and a linked probe module (see docs/TOOLCHAIN.md,
// first-check (b)):
//
//   * The throw primitive is __builtin_wasm_throw with tag index
//     0, which is the C++ exception tag __cpp_exception. Its
//     payload is the _Unwind_Exception pointer itself (the unwind
//     header embedded at the end of lithoabi_eh_header), NOT the
//     adjusted object pointer. LithoAbi's __cxa_begin_catch
//     already expects exactly that pointer.
//   * Compiler-generated landing pads do the selection dance
//     themselves: a typed pad stashes the LSDA address and its
//     landing pad index into a thread-local context object the
//     RUNTIME must define (__wasm_lpad_context, layout below),
//     calls _Unwind_CallPersonality(payload), then compares the
//     context's selector field against the ttype index of its own
//     catch clause. Selector 0 (or a non-matching index) means "not
//     mine", and the pad rethrows to the caller. catch(...)
//     pads never consult the personality: they match by
//     construction. Cleanup pads are catch_all pads and likewise
//     never consult it.
//   * _Unwind_CallPersonality resets the selector to 0 and calls
//     the personality entry, whose wasm name is
//     __gxx_personality_wasm0 (NOT __gxx_personality_v0). The
//     portable __gxx_personality_v0 in personality.cc sits behind
//     it, fed through the adapter context defined below.
//   * clang's wasm LSDA tables use a DIFFERENT envelope grammar
//     from the DWARF form the portable procedure speaks (both
//     byte details the Abi doc flagged live here):
//       - the TType base value is a ULEB128 counted from the byte
//         AFTER the encoded value (libcxxabi: classInfo = lsda +
//         classInfoOffset, with lsda already advanced past the
//         value), not from the LSDA start;
//       - the call-site table holds one (landingPad, action)
//         ULEB128 PAIR per personality-consulted pad, selected by
//         1-based pad index, not (start, length, lpad, action)
//         ranges.
//     Rather than teach personality.cc a second grammar, this
//     back-end TRANSLATES the envelope: on each personality call
//     it rebuilds the LSDA in a scratch buffer in the form the
//     portable procedure reads (4-field call sites, TType base as
//     an offset from the buffer start, TType entries declared
//     udata4, which is the same 4-byte size as the absptr entries
//     clang emits on wasm32). The action table and the TType table
//     bytes are copied verbatim: chains, indices and typeinfo
//     pointers are untouched, only the envelope is re-encoded.
//     The translation is per-call malloc scratch; exceptions are
//     not a hot path.
//   * KNOWN LIMITATION, recorded honestly: a single try block that
//     mixes typed clauses with a trailing catch(...) encodes the
//     catch-all as a POSITIVE ttype index whose table entry is
//     null. The portable walker's ttype resolution maps a null
//     entry to "no match", so that shape would miss its catch-all
//     (a standalone catch(...) try, the common shape, never
//     consults the personality and works). Fixing it belongs to a
//     LithoAbi follow-up in personality.cc's walk_chain, not to
//     this back-end.
//   * Terminate contract: nothing here traps the host silently.
//     An exception no pad selects is rethrown pad by pad and
//     escapes the module as a wasm exception to the host, per the
//     standing caught-inside / uncaught-fatal contract; a delivery
//     that reaches the back-end with nowhere to go routes through
//     lithoabi_backend_fatal_uncaught, which is loud and ends in
//     lithosys_abort like every terminate path.

#include "lithoabi_wasm_eh.h"
#include "lithoabi_internal.h"

#include <stdlib.h> // malloc / free for the LSDA translation scratch
#include <string.h> // memcpy

#ifdef __wasm__

// The landing pad context the compiler's generated pads write to.
// Layout fixed by the compiler runtime ABI (libunwind's
// _Unwind_LandingPadContext): three pointer-sized fields. The
// generated code takes its address as __tls_base + TLS offset, so
// it MUST be a thread_local definition with exactly this name.
extern "C" {
struct _Unwind_LandingPadContext {
  uintptr_t lpad_index;
  uintptr_t lsda;
  uintptr_t selector;
};
__thread _Unwind_LandingPadContext __wasm_lpad_context;
}

namespace __cxxabiv1 {

//===------------------------------------------------------------------===
// Adapter context: what the portable procedure's accessors read.
// Built on the stack of __gxx_personality_wasm0 per call.
//===------------------------------------------------------------------===

struct lithoabi_personality_context {
  const uint8_t *lsda; // translated LSDA (scratch owned by caller)
  std::size_t lsda_size;
  uintptr_t ip; // landing pad index (walker convention)
  _Unwind_Exception *unwind;
  int installed_switch; // recorded by install(), for completeness
  void *installed_adjusted;
  bool installed;
};

//===------------------------------------------------------------------===
// LSDA envelope translation (clang wasm grammar -> walker grammar)
//===------------------------------------------------------------------===

namespace {

bool rd_uleb(const uint8_t *&p, const uint8_t *end, uint64_t *out) {
  uint64_t v = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t b = *p++;
    v |= (uint64_t)(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      *out = v;
      return true;
    }
    shift += 7;
    if (shift >= 64) return false;
  }
  return false;
}

bool wr_uleb(uint8_t *&p, uint64_t v) {
  for (;;) {
    uint8_t b = (uint8_t)(v & 0x7f);
    v >>= 7;
    if (v) {
      *p++ = (uint8_t)(b | 0x80);
    } else {
      *p++ = b;
      return true;
    }
  }
}

void wr_u32le(uint8_t *&p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
  p += 4;
}

// Translate one clang-emitted wasm LSDA into walker grammar.
// Returns a malloc'd buffer (caller frees) and its size, or NULL
// on any shape this back-end does not recognize. A NULL here makes
// the personality decline the pad (CONTINUE_UNWIND): the
// exception escapes rather than being mis-caught, and a diag line
// names the failure.
uint8_t *translate_lsda(const uint8_t *src, std::size_t *out_size) {
  // The raw LSDA has no recorded length of its own. Bound every
  // read by the end of memory: the tables are self-terminating
  // well inside it, and the last field we need (the TType table
  // end) is computed from the header itself.
  const std::size_t mem_bytes = __builtin_wasm_memory_size(0) * 65536u;
  const uint8_t *mem_end = (const uint8_t *)(uintptr_t)mem_bytes;
  const uint8_t *p = src;

  if (p >= mem_end) return NULL;
  uint8_t lp_enc = *p++;
  if (lp_enc != 0xff) {
    // clang omits the landing pad base for wasm (libcxxabi marks
    // it unused in this model). Anything else is unrecognized.
    return NULL;
  }
  if (p >= mem_end) return NULL;
  uint8_t tt_enc = *p++;
  if (tt_enc != 0x00) {
    // absptr entries (4 bytes on wasm32) are what clang emits and
    // what the translation re-declares as udata4 (same size).
    return NULL;
  }
  uint64_t tt_off = 0;
  if (!rd_uleb(p, mem_end, &tt_off)) return NULL;
  // TType table END, per the libcxxabi rule: the offset counts
  // from the byte after the encoded value.
  const uint8_t *ttype_end = p + tt_off;
  if (ttype_end > mem_end) return NULL;

  if (p >= mem_end) return NULL;
  uint8_t cs_enc = *p++;
  if (cs_enc != 0x01) return NULL; // pairs are ULEB128 in this model
  uint64_t cs_len = 0;
  if (!rd_uleb(p, mem_end, &cs_len)) return NULL;
  const uint8_t *cs_begin = p;
  const uint8_t *action_begin = cs_begin + cs_len;
  if (action_begin > ttype_end) return NULL;

  // Parse the (landingPad, action) pairs first so the translated
  // size is known exactly.
  struct Pair {
    uint64_t lpad;
    uint64_t action;
  };
  Pair pairs[256];
  std::size_t npairs = 0;
  const uint8_t *q = cs_begin;
  while (q < action_begin) {
    if (npairs >= 256) return NULL;
    if (!rd_uleb(q, action_begin, &pairs[npairs].lpad)) return NULL;
    if (!rd_uleb(q, action_begin, &pairs[npairs].action)) return NULL;
    ++npairs;
  }
  if (q != action_begin) return NULL;

  // Translated layout:
  //   header: lp omit (1) + ttype enc udata4 (1) + ttype base
  //           as a 4-byte value counted from the buffer start (4)
  //           + call-site enc uleb (1) + call-site length uleb
  //   call sites: per pair (start=index, length=1,
  //               lpad=pair.lpad+1, action=pair.action), all uleb
  //   then the bytes from action_begin to ttype_end verbatim
  //   (action table followed by the TType table, end-aligned).
  const std::size_t tail_len = (std::size_t)(ttype_end - action_begin);
  std::size_t cs_translated = 0;
  for (std::size_t i = 0; i < npairs; ++i) {
    uint64_t fields[4] = {(uint64_t)i, 1, pairs[i].lpad + 1,
                          pairs[i].action};
    for (int k = 0; k < 4; ++k) {
      uint64_t v = fields[k];
      do {
        ++cs_translated;
        v >>= 7;
      } while (v);
    }
  }
  // cs_len field width: a single uleb of cs_translated (it does
  // not feed back into cs_translated itself).
  uint8_t probe_len[8];
  uint8_t *pp = probe_len;
  wr_uleb(pp, cs_translated);
  const std::size_t cs_len_width = (std::size_t)(pp - probe_len);
  const std::size_t header_len = 1 + 1 + 4 + 1 + cs_len_width;
  const std::size_t total = header_len + cs_translated + tail_len;
  const uint32_t ttype_base_new = (uint32_t)total;

  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return NULL;
  uint8_t *w = buf;
  *w++ = 0xff; // lp encoding: omit
  *w++ = 0x03; // ttype encoding: udata4 (entries stay 4 bytes)
  wr_u32le(w, ttype_base_new);
  *w++ = 0x01; // call-site encoding: uleb128
  wr_uleb(w, cs_translated);
  for (std::size_t i = 0; i < npairs; ++i) {
    wr_uleb(w, (uint64_t)i);
    wr_uleb(w, 1);
    wr_uleb(w, pairs[i].lpad + 1);
    wr_uleb(w, pairs[i].action);
  }
  memcpy(w, action_begin, tail_len);
  w += tail_len;
  *out_size = (std::size_t)(w - buf);
  return buf;
}

} // namespace

//===------------------------------------------------------------------===
// Personality accessors over the adapter (the seam's contract).
//===------------------------------------------------------------------===

const uint8_t *lithoabi_personality_lsda(lithoabi_personality_context *ctx,
                                         std::size_t *out_size) {
  if (ctx == NULL) return NULL;
  if (out_size) *out_size = ctx->lsda_size;
  return ctx->lsda;
}

const std::type_info *
lithoabi_personality_ttype(lithoabi_personality_context *ctx,
                           const uint8_t *lsda_base, uintptr_t encoded_ref) {
  (void)ctx;
  (void)lsda_base;
  // absptr grammar: the encoded value IS the typeinfo address.
  if (encoded_ref == 0) return NULL;
  return (const std::type_info *)encoded_ref;
}

_Unwind_Exception *
lithoabi_personality_unwind_header(lithoabi_personality_context *ctx) {
  return ctx ? ctx->unwind : NULL;
}

uintptr_t lithoabi_personality_ip(lithoabi_personality_context *ctx) {
  // Walker grammar: the translated call site for pad k covers
  // [k, k+1), so the ip is the pad index itself.
  return ctx ? ctx->ip : 0;
}

void lithoabi_personality_install(lithoabi_personality_context *ctx,
                                  int switch_value, void *adjusted_ptr) {
  if (!ctx) return;
  ctx->installed_switch = switch_value;
  ctx->installed_adjusted = adjusted_ptr;
  ctx->installed = true;
}

//===------------------------------------------------------------------===
// Throw / rethrow / caught / uncaught.
//===------------------------------------------------------------------===

[[noreturn]] void lithoabi_backend_throw(lithoabi_eh_header *header) {
  // Tag index 0 is __cpp_exception; the payload is the unwind
  // header pointer (libunwind's _Unwind_RaiseException does
  // exactly this).
  __builtin_wasm_throw(0, (void *)&header->unwindHeader);
  __builtin_unreachable();
}

[[noreturn]] void lithoabi_backend_rethrow(lithoabi_eh_header *header) {
  // A rethrow continues the SAME exception. Throwing the same
  // unwind header again from the handler body unwinds to the
  // next enclosing pad, which is the observable semantics the
  // probe verifies (inner catch, rethrow, outer catch).
  __builtin_wasm_throw(0, (void *)&header->unwindHeader);
  __builtin_unreachable();
}

lithoabi_eh_header *lithoabi_backend_caught_header() {
  // No consumer in this model: the generated pads pass the unwind
  // payload straight to __cxa_begin_catch. If anything ever calls
  // this, it is a glue error, so say so loudly.
  lithoabi_terminate_now(
      "lithoabi: backend_caught_header has no wasm-model consumer");
  return NULL;
}

[[noreturn]] void
lithoabi_backend_fatal_uncaught(lithoabi_eh_header *header) {
  const char *name = "<unknown>";
  if (header != NULL && header->exceptionType != NULL &&
      header->exceptionType->name() != NULL) {
    name = header->exceptionType->name();
  }
  lithoabi_diag("lithoabi: uncaught exception escaped (type ", name,
                ") ... fatal per the caught-inside contract\n");
  lithoabi_terminate_now("uncaught exception");
}

} // namespace __cxxabiv1

using namespace __cxxabiv1;

//===------------------------------------------------------------------===
// The personality entries the compiler runtime calls.
//===------------------------------------------------------------------===

extern "C" __attribute__((used)) _Unwind_Reason_Code
__gxx_personality_wasm0(int version, _Unwind_Action actions,
                        uint64_t exception_class, void *unwind_exception,
                        void *raw_context) {
  _Unwind_Exception *ue = (_Unwind_Exception *)unwind_exception;
  const _Unwind_LandingPadContext *lpad =
      (const _Unwind_LandingPadContext *)raw_context;
  if (!lpad || !lpad->lsda) return _URC_CONTINUE_UNWIND;

  std::size_t translated_size = 0;
  uint8_t *translated = translate_lsda(
      (const uint8_t *)(uintptr_t)lpad->lsda, &translated_size);
  if (!translated) {
    lithoabi_diag("lithoabi: wasm LSDA translation failed; pad declined");
    return _URC_CONTINUE_UNWIND;
  }

  lithoabi_personality_context adapter;
  adapter.lsda = translated;
  adapter.lsda_size = translated_size;
  adapter.ip = lpad->lpad_index;
  adapter.unwind = ue;
  adapter.installed_switch = 0;
  adapter.installed_adjusted = NULL;
  adapter.installed = false;

  _Unwind_Reason_Code rc = __gxx_personality_v0(
      version, actions, exception_class, ue, &adapter);

  if (rc == _URC_HANDLER_FOUND) {
    // The generated pad gates on the TTYPE INDEX of the matched
    // clause (LKProbeError pads gate on 1, the base class pad on
    // 2, in the probe module). decide() recorded the selecting
    // action record; its first field is that index. A catch-all
    // outcome in walker grammar comes from a zero index, which
    // leaves the selector 0 and the pad rethrows: also the right
    // answer for a cleanup record, the only zero-index record the
    // wasm grammar produces in a chain.
    lithoabi_eh_header *header = lithoabi_header_of_unwind(ue);
    int64_t index = 0;
    if (header && header->actionRecord) {
      const uint8_t *p = (const uint8_t *)header->actionRecord;
      // Read one SLEB128.
      uint64_t v = 0;
      unsigned shift = 0;
      uint8_t b;
      do {
        b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
      } while (b & 0x40);
      if (b & 0x40) v |= ~(uint64_t)0 << shift;
      index = (int64_t)v;
    }
    ((_Unwind_LandingPadContext *)lpad)->selector =
        (uintptr_t)(index > 0 ? index : 0);
  }

  free(translated);
  return rc;
}

extern "C" _Unwind_Reason_Code
_Unwind_CallPersonality(void *exception_ptr) {
  // The upstream runtime's shape: reset the selector, then ask
  // the personality in search phase. The context pointer the
  // runtime model hands over IS the landing pad context.
  __wasm_lpad_context.selector = 0;
  _Unwind_Exception *ue = (_Unwind_Exception *)exception_ptr;
  return __gxx_personality_wasm0(1, _UA_SEARCH_PHASE, ue->exception_class,
                                 ue, (void *)&__wasm_lpad_context);
}

#endif // __wasm__
