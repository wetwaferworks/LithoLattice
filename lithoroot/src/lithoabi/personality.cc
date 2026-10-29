/* personality.cc ... the personality decision procedure.
 *
 * Portable LSDA (language-specific data area) walking plus the
 * two-phase handler decision, written against the seam accessors
 * in lithoabi_wasm_eh.h: the backend supplies the LSDA bytes, the
 * instruction pointer in call-site coordinates, the unwind header,
 * TType resolution, and the install slot; everything about the
 * table's meaning is decided here.
 *
 * The LSDA layout is the Itanium / DWARF exception-table layout:
 * a header (landing-pad base encoding and value, TType base
 * encoding and value, call-site encoding, call-site table length),
 * the call-site table (start, length, landing pad, action offset,
 * the first three in the call-site encoding, the action offset
 * always uleb128 and stored as table offset plus one), and the
 * action table (chains of records: sleb128 TType index, sleb128
 * displacement to the next record). A TType index above zero names
 * a catch clause's type, zero is a catch-all (and, in the cleanup
 * phase, simply "the landing pad has work"), and a negative index
 * is an exception-specification filter.
 *
 * TType entries are read from the end of the TType table backwards:
 * the header's TType base is consumed as a byte offset from the
 * LSDA start to the table's end, and entry i sits entry_size bytes
 * further back per step. That is the layout the reference tables
 * and the wasm exception tables use. TType entry encodings must
 * have a fixed size (udata/sdata 2/4/8 or absptr); variable-size
 * encodings cannot be indexed backwards and are treated as "no
 * resolution" if a record ever needs one.
 *
 * Two byte-level details are implemented in their documented form
 * and flagged for the toolchain piece to confirm against real
 * emitted tables, in the same class as the throw tag's spelling:
 * the action chain's displacement is read as relative to the start
 * of the displacement field itself, and a filter's type list is
 * read as uleb128 TType indices, zero-terminated, starting at the
 * action table start plus the negated index. Exception
 * specifications cannot arise from C++17 engine code (dynamic
 * exception specifications are removed from the language), so the
 * filter path is completeness, not a load-bearing behavior.
 *
 * Foreign exceptions (a different exception class) never match a
 * catch here; their cleanups still run, per the model.
 */

#include <typeinfo>
#include <cstddef>
#include <cstdint>

#include "lithoabi_internal.h"
#include "lithoabi_wasm_eh.h"
#include "typeinfo_layout.h"

namespace __cxxabiv1 {
namespace {

/* Exception-header encoding formats (the low nibble of an encoding
 * byte) and the omit marker. Base modifiers in the high nibble are
 * not applied by this walker: the seam defines the instruction
 * pointer in the call-site table's own coordinate space, and TType
 * raw references are handed to the backend resolver untouched. */
constexpr uint8_t kFmtAbsPtr = 0x00;
constexpr uint8_t kFmtUleb128 = 0x01;
constexpr uint8_t kFmtUdata2 = 0x02;
constexpr uint8_t kFmtUdata4 = 0x03;
constexpr uint8_t kFmtUdata8 = 0x04;
constexpr uint8_t kFmtSleb128 = 0x09;
constexpr uint8_t kFmtSdata2 = 0x0a;
constexpr uint8_t kFmtSdata4 = 0x0b;
constexpr uint8_t kFmtSdata8 = 0x0c;
constexpr uint8_t kFmtOmit = 0xff;

struct reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
};

bool read_byte(reader& r, uint8_t* out) {
  if (r.p >= r.end) {
    r.ok = false;
    return false;
  }
  *out = *r.p++;
  return true;
}

bool read_uleb(reader& r, uintptr_t* out) {
  uintptr_t value = 0;
  unsigned shift = 0;
  for (;;) {
    uint8_t byte = 0;
    if (!read_byte(r, &byte)) return false;
    value |= static_cast<uintptr_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) break;
    shift += 7;
    if (shift >= sizeof(uintptr_t) * 8) {
      r.ok = false;
      return false;
    }
  }
  *out = value;
  return true;
}

bool read_sleb(reader& r, intptr_t* out) {
  uintptr_t value = 0;
  unsigned shift = 0;
  uint8_t byte = 0;
  for (;;) {
    if (!read_byte(r, &byte)) return false;
    value |= static_cast<uintptr_t>(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) break;
    if (shift >= sizeof(uintptr_t) * 8) {
      r.ok = false;
      return false;
    }
  }
  if (shift < sizeof(uintptr_t) * 8 && (byte & 0x40)) {
    value |= ~uintptr_t(0) << shift;
  }
  *out = static_cast<intptr_t>(value);
  return true;
}

std::size_t fixed_format_size(uint8_t format) {
  switch (format) {
    case kFmtAbsPtr:
      return sizeof(void*);
    case kFmtUdata2:
    case kFmtSdata2:
      return 2;
    case kFmtUdata4:
    case kFmtSdata4:
      return 4;
    case kFmtUdata8:
    case kFmtSdata8:
      return 8;
    default:
      return 0; /* variable-size formats: uleb128, sleb128 */
  }
}

bool read_fixed(reader& r, uint8_t format, std::size_t size,
                uintptr_t* out) {
  if (static_cast<std::size_t>(r.end - r.p) < size) {
    r.ok = false;
    return false;
  }
  uintptr_t value = 0;
  for (std::size_t i = 0; i < size; ++i) {
    value |= static_cast<uintptr_t>(r.p[i]) << (i * 8);
  }
  r.p += size;
  if ((format == kFmtSdata2 || format == kFmtSdata4 ||
       format == kFmtSdata8) &&
      size < sizeof(uintptr_t)) {
    const uintptr_t sign = uintptr_t(1) << (size * 8 - 1);
    if (value & sign) value |= ~uintptr_t(0) << (size * 8);
  }
  *out = value;
  return true;
}

bool read_encoded(reader& r, uint8_t encoding, uintptr_t* out) {
  const uint8_t format = encoding & 0x0f;
  if (format == kFmtUleb128) return read_uleb(r, out);
  if (format == kFmtSleb128) {
    intptr_t v = 0;
    if (!read_sleb(r, &v)) return false;
    *out = static_cast<uintptr_t>(v);
    return true;
  }
  const std::size_t size = fixed_format_size(format);
  if (size == 0) {
    r.ok = false;
    return false;
  }
  return read_fixed(r, format, size, out);
}

struct lsda_info {
  const uint8_t* base = nullptr;
  std::size_t size = 0;
  uint8_t ttype_encoding = kFmtOmit;
  uintptr_t ttype_base = 0; /* byte offset from base to table end */
  uint8_t callsite_encoding = 0;
  const uint8_t* callsite_table = nullptr;
  std::size_t callsite_size = 0;
  const uint8_t* action_table = nullptr;
};

bool parse_lsda(const uint8_t* base, std::size_t size, lsda_info* out) {
  reader r{base, base + size, true};
  uint8_t lp_encoding = 0;
  if (!read_byte(r, &lp_encoding)) return false;
  if (lp_encoding != kFmtOmit) {
    uintptr_t ignored = 0;
    if (!read_encoded(r, lp_encoding, &ignored)) return false;
  }
  if (!read_byte(r, &out->ttype_encoding)) return false;
  if (out->ttype_encoding != kFmtOmit) {
    if (!read_encoded(r, out->ttype_encoding, &out->ttype_base)) {
      return false;
    }
  }
  if (!read_byte(r, &out->callsite_encoding)) return false;
  uintptr_t cs_len = 0;
  if (!read_uleb(r, &cs_len)) return false;
  if (static_cast<uintptr_t>(r.end - r.p) < cs_len) return false;
  out->base = base;
  out->size = size;
  out->callsite_table = r.p;
  out->callsite_size = static_cast<std::size_t>(cs_len);
  out->action_table = r.p + cs_len;
  return true;
}

/* Read one raw TType table reference: entry "index" (1-based) of
 * the table whose end is the header's TType base. The raw value is
 * the backend resolver's business; this only fetches it. */
bool read_ttype_ref(const lsda_info& info, uintptr_t index,
                    uintptr_t* out_raw) {
  if (info.ttype_encoding == kFmtOmit || index == 0) return false;
  const std::size_t entry_size =
      fixed_format_size(info.ttype_encoding & 0x0f);
  if (entry_size == 0) return false;
  if (info.ttype_base > info.size) return false;
  const std::size_t back = static_cast<std::size_t>(index) * entry_size;
  if (back > info.ttype_base) return false;
  const uint8_t* at = info.base + (info.ttype_base - back);
  uintptr_t value = 0;
  for (std::size_t i = 0; i < entry_size; ++i) {
    value |= static_cast<uintptr_t>(at[i]) << (i * 8);
  }
  *out_raw = value;
  return true;
}

struct call_site {
  bool present = false;
  uintptr_t landing_pad = 0;
  uintptr_t action_disp = 0; /* 0 = no action; else offset + 1 */
};

bool find_call_site(const lsda_info& info, uintptr_t ip, call_site* out) {
  reader r{info.callsite_table, info.callsite_table + info.callsite_size,
           true};
  while (r.p < r.end) {
    uintptr_t start = 0, length = 0, lp = 0, action = 0;
    if (!read_encoded(r, info.callsite_encoding, &start)) return false;
    if (!read_encoded(r, info.callsite_encoding, &length)) return false;
    if (!read_encoded(r, info.callsite_encoding, &lp)) return false;
    if (!read_uleb(r, &action)) return false;
    if (ip >= start && ip < start + length) {
      out->present = true;
      out->landing_pad = lp;
      out->action_disp = action;
      return true;
    }
  }
  return r.ok;
}

/* One pass over the action chain headed at byte offset
 * (action_disp - 1) in the action table. In select mode (search
 * phase) the chain is matched against the thrown type; the first
 * selecting record wins and its outcome is reported. In presence
 * mode (cleanup phase) the chain is only checked for existence. */
enum class chain_outcome {
  kNone,        /* no record selected */
  kCatch,       /* a catch clause selected; adjusted is set */
  kCatchAll,    /* a catch-all selected; adjusted is the object */
  kFilter,      /* an exception specification selected */
  kPresent      /* presence mode: at least one record exists */
};

struct chain_result {
  chain_outcome outcome = chain_outcome::kNone;
  const uint8_t* record = nullptr; /* the selecting record */
  void* adjusted = nullptr;
};

chain_result walk_chain(const lsda_info& info, lithoabi_personality_context* ctx,
                        uintptr_t action_disp, lithoabi_eh_header* header,
                        bool presence_only, bool foreign) {
  chain_result result;
  if (action_disp == 0) return result;
  /* The action table span: from the action table start to the end
   * of the LSDA. parse_lsda guarantees the action table starts
   * inside the LSDA, so this is never negative. */
  const std::size_t action_bytes =
      static_cast<std::size_t>((info.base + info.size) - info.action_table);
  /* Audit fix D-F3: validate the entry offset two-sided BEFORE
   * forming the entry pointer. The old code formed
   * action_table + (action_disp - 1) from a table-supplied ULEB
   * and checked only the upper bound, so a large offset wrapped
   * the pointer below the LSDA and the walk read "records" out of
   * unrelated memory (on wasm32, where the read cannot fault). */
  if (action_disp - 1 >= action_bytes) return result;
  const uint8_t* at = info.action_table + (action_disp - 1);
  bool saw_any = false;
  /* Audit fix D-F2: bound the walk. Every record carries at least
   * two bytes (two sleb128 fields), so a chain that visits more
   * records than the table has byte-pairs is not a chain: it is a
   * cycle or a corruption, and the personality must return (with
   * no selection) rather than hang inside unwinding. */
  std::size_t steps_left = action_bytes / 2 + 1;
  while (at != nullptr) {
    if (steps_left == 0) break;
    steps_left -= 1;
    reader r{at, info.base + info.size, true};
    intptr_t ttype_index = 0;
    if (!read_sleb(r, &ttype_index)) break;
    const uint8_t* disp_field = r.p;
    intptr_t disp = 0;
    if (!read_sleb(r, &disp)) break;
    saw_any = true;
    if (presence_only) {
      result.outcome = chain_outcome::kPresent;
      result.record = at;
      return result;
    }
    if (!foreign) {
      if (ttype_index == 0) {
        result.outcome = chain_outcome::kCatchAll;
        result.record = at;
        result.adjusted = lithoabi_object_of_header(header);
        return result;
      }
      if (ttype_index > 0) {
        uintptr_t raw = 0;
        const std::type_info* catch_type = nullptr;
        if (read_ttype_ref(info, static_cast<uintptr_t>(ttype_index),
                           &raw)) {
          catch_type =
              lithoabi_personality_ttype(ctx, info.base, raw);
        }
        if (catch_type != nullptr && header->exceptionType != nullptr) {
          void* adjusted = lithoabi_object_of_header(header);
          void* scratch = adjusted;
          if (catch_type->__do_catch(header->exceptionType, &scratch,
                                     0)) {
            result.outcome = chain_outcome::kCatch;
            result.record = at;
            result.adjusted = scratch;
            return result;
          }
        }
      }
      if (ttype_index < 0) {
        /* Exception specification: selects when the thrown type
         * matches none of the listed types. The list offset gets
         * the same two-sided treatment as the chain entry (audit
         * fix D-F3): the magnitude is formed without negating a
         * signed value (INTPTR_MIN has no positive counterpart)
         * and is checked against the action table span before any
         * pointer is formed. An out-of-span list is unreadable, so
         * nothing is listed and the filter selects, exactly as the
         * old upper-bound check behaved for offsets past the end. */
        const uintptr_t list_off =
            static_cast<uintptr_t>(-(ttype_index + 1)) + 1;
        bool listed = false;
        if (list_off < action_bytes) {
          const uint8_t* list = info.action_table + list_off;
          reader lr{list, info.base + info.size, true};
          for (;;) {
            uintptr_t idx = 0;
            if (!read_uleb(lr, &idx) || idx == 0) break;
            uintptr_t raw = 0;
            const std::type_info* listed_type = nullptr;
            if (read_ttype_ref(info, idx, &raw)) {
              listed_type =
                  lithoabi_personality_ttype(ctx, info.base, raw);
            }
            if (listed_type != nullptr &&
                header->exceptionType != nullptr &&
                *listed_type == *header->exceptionType) {
              listed = true;
              break;
            }
          }
        }
        if (!listed) {
          result.outcome = chain_outcome::kFilter;
          result.record = at;
          result.adjusted = lithoabi_object_of_header(header);
          return result;
        }
      }
    }
    if (disp == 0) break;
    at = disp_field + disp;
    if (at < info.action_table || at >= info.base + info.size) break;
  }
  (void)saw_any;
  return result;
}

_Unwind_Reason_Code decide(_Unwind_Action actions, uint64_t exception_class,
                           lithoabi_personality_context* ctx) {
  std::size_t lsda_size = 0;
  const uint8_t* lsda_bytes = lithoabi_personality_lsda(ctx, &lsda_size);
  if (lsda_bytes == nullptr) return _URC_CONTINUE_UNWIND;
  lsda_info info;
  if (!parse_lsda(lsda_bytes, lsda_size, &info)) return _URC_FAILURE;

  _Unwind_Exception* ue = lithoabi_personality_unwind_header(ctx);
  if (ue == nullptr) return _URC_FAILURE;
  lithoabi_eh_header* header = lithoabi_header_of_unwind(ue);
  const bool foreign = exception_class != lithoabi_cpp_exception_class();
  const uintptr_t ip = lithoabi_personality_ip(ctx);

  call_site site;
  if (!find_call_site(info, ip, &site)) return _URC_FAILURE;
  if (!site.present || site.landing_pad == 0) return _URC_CONTINUE_UNWIND;

  const bool search_phase = (actions & _UA_SEARCH_PHASE) != 0;
  const bool handler_frame = (actions & _UA_HANDLER_FRAME) != 0;
  const bool force = (actions & _UA_FORCE_UNWIND) != 0;

  if (search_phase && !force) {
    chain_result r = walk_chain(info, ctx, site.action_disp, header,
                                /*presence_only=*/false, foreign);
    if (r.outcome == chain_outcome::kCatch ||
        r.outcome == chain_outcome::kCatchAll ||
        r.outcome == chain_outcome::kFilter) {
      header->actionRecord = reinterpret_cast<const char*>(r.record);
      header->languageSpecificData =
          reinterpret_cast<const char*>(info.base);
      header->handlerSwitchValue = static_cast<int>(site.action_disp);
      header->adjustedPtr = r.adjusted;
      lithoabi_personality_install(ctx, static_cast<int>(site.action_disp),
                                   r.adjusted);
      return _URC_HANDLER_FOUND;
    }
    return _URC_CONTINUE_UNWIND;
  }

  /* Cleanup phase (also the force-unwind walk): the landing pad
   * runs if the frame has any action at all. For the handler frame
   * itself, re-run the selection so the install carries the same
   * switch value and adjusted pointer the search phase chose. */
  if (handler_frame && !force) {
    chain_result r = walk_chain(info, ctx, site.action_disp, header,
                                /*presence_only=*/false, foreign);
    if (r.outcome == chain_outcome::kCatch ||
        r.outcome == chain_outcome::kCatchAll ||
        r.outcome == chain_outcome::kFilter) {
      header->actionRecord = reinterpret_cast<const char*>(r.record);
      header->languageSpecificData =
          reinterpret_cast<const char*>(info.base);
      header->handlerSwitchValue = static_cast<int>(site.action_disp);
      header->adjustedPtr = r.adjusted;
      lithoabi_personality_install(ctx, static_cast<int>(site.action_disp),
                                   r.adjusted);
      return _URC_INSTALL_CONTEXT;
    }
    return _URC_FAILURE;
  }

  chain_result r = walk_chain(info, ctx, site.action_disp, header,
                              /*presence_only=*/true, foreign);
  if (r.outcome == chain_outcome::kPresent) {
    lithoabi_personality_install(ctx, static_cast<int>(site.action_disp),
                                 nullptr);
    return _URC_HANDLER_FOUND;
  }
  return _URC_CONTINUE_UNWIND;
}

} /* namespace */

/* The personality entry point. The signature carries the seam
 * context in place of the target's unwind context: adapting the
 * compiler's real context type to the lithoabi_personality_*
 * accessors is the backend's job (see lithoabi_wasm_eh.h), so this
 * entry is already in its final shape for both backends. */
_Unwind_Reason_Code __gxx_personality_v0(int version,
                                         _Unwind_Action actions,
                                         uint64_t exception_class,
                                         _Unwind_Exception* unwind_exception,
                                         lithoabi_personality_context* ctx) {
  if (version != 1 || ctx == nullptr) return _URC_FAILURE;
  if (lithoabi_personality_unwind_header(ctx) != unwind_exception) {
    return _URC_FAILURE;
  }
  return decide(actions, exception_class, ctx);
}

} /* namespace __cxxabiv1 */
