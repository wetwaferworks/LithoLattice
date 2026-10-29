/* typeinfo.cc ... std::type_info behavior, the __cxxabiv1
 * type_info classes, catch matching (__do_catch / __do_upcast), the
 * dynamic_cast walk, and the fundamental typeinfo objects.
 *
 * Everything here implements the Itanium ABI's runtime semantics
 * from the specification: the class layouts come from
 * typeinfo_layout.h (which the compiler's emitted objects share),
 * and the algorithms are the standard ones: catch matching walks
 * the thrown type's public bases; pointer catching applies the
 * qualification rules level by level; dynamic_cast resolves the
 * dynamic type from the vtable, then looks for an unambiguous
 * public destination sub-object inside the whole object, with the
 * plain upcast as the fallback.
 *
 * Two documented simplifications against the full reference
 * semantics, both in pointer catching (docs/LITHOABI.md has the
 * reasoning): member-pointer catching requires identical member
 * types and identical context classes rather than the full
 * member-pointer conversion lattice, and the multi-level
 * qualification rule is implemented in its standard form (quals may
 * be added at a deeper level only when every shallower catch-side
 * level carries const).
 */

#include <typeinfo>
#include <cstddef>
#include <cstdint>

#include "typeinfo_layout.h"
#include "lithoabi_internal.h"

namespace std {

type_info::~type_info() = default;

bool type_info::__is_pointer_p() const { return false; }
bool type_info::__is_function_p() const { return false; }

bool type_info::__do_catch(const type_info* thrown_type, void**,
                           unsigned) const {
  return *this == *thrown_type;
}

bool type_info::__do_upcast(const __cxxabiv1::__class_type_info*, void**) const {
  return false;
}

} /* namespace std */

namespace __cxxabiv1 {

__fundamental_type_info::~__fundamental_type_info() = default;
__array_type_info::~__array_type_info() = default;
__enum_type_info::~__enum_type_info() = default;
__function_type_info::~__function_type_info() = default;
__pbase_type_info::~__pbase_type_info() = default;
__pointer_type_info::~__pointer_type_info() = default;
__pointer_to_member_type_info::~__pointer_to_member_type_info() = default;
__class_type_info::~__class_type_info() = default;
__si_class_type_info::~__si_class_type_info() = default;
__vmi_class_type_info::~__vmi_class_type_info() = default;

bool __function_type_info::__is_function_p() const { return true; }
bool __pointer_type_info::__is_pointer_p() const { return true; }

/* ================================================================
 * Pointer catch matching.
 *
 * The __flags word of a pbase type carries the cv-qualifiers of its
 * pointee in the encoding the compiler emits (probed against host
 * g++ typeinfo emission during the LithoAbi test pass, matching
 * __pbase_type_info::__masks in typeinfo_layout.h): const in bit 0,
 * volatile in bit 1, restrict in bit 2. Only these bits take part
 * in the qualification rules; other bits are not qualifiers and are
 * masked out of every comparison below.
 */
namespace {
constexpr unsigned int kQualConst = __pbase_type_info::__const_mask;
constexpr unsigned int kQualVolatile = __pbase_type_info::__volatile_mask;
constexpr unsigned int kQualRestrict = __pbase_type_info::__restrict_mask;
constexpr unsigned int kQualMask = kQualConst | kQualVolatile | kQualRestrict;

bool quals_subset(unsigned int thrown_quals, unsigned int catch_quals) {
  return (thrown_quals & ~catch_quals & kQualMask) == 0;
}

/* One pointer level of the match. catch_type is the handler's
 * pbase type, thrown_type the thrown one; both are known pbase.
 * thrown_obj points at the pointer VALUE inside the exception
 * object (for level 0, the object itself). level counts pointer
 * levels from the outside; const_prefix is the multi-level rule's
 * state: every shallower catch-side pointee so far carried const.
 * On a class upcast match the pointee pointer stored at thrown_obj
 * is adjusted in place (the caller passes a scratch copy). */
bool pointer_level_catch(const __pbase_type_info* catch_type,
                         const __pbase_type_info* thrown_type,
                         void** thrown_obj, unsigned level,
                         bool const_prefix) {
  const unsigned int catch_q = catch_type->__flags & kQualMask;
  const unsigned int thrown_q = thrown_type->__flags & kQualMask;
  if (level == 0) {
    if (!quals_subset(thrown_q, catch_q)) return false;
  } else {
    if (thrown_q != catch_q) {
      /* A deeper level may change qualifiers only by adding them,
       * and only under a const-carrying prefix. */
      if (!const_prefix || !quals_subset(thrown_q, catch_q)) return false;
    }
  }
  const bool next_prefix = const_prefix && (catch_q & kQualConst);

  const std::type_info* catch_pointee = catch_type->__pointee;
  const std::type_info* thrown_pointee = thrown_type->__pointee;

  if (*catch_pointee == *thrown_pointee) return true;

  /* Catching as void*: any object pointer converts, subject to the
   * qualifier rule already applied above. */
  if (*catch_pointee == typeid(void)) return true;

  const auto* catch_pbase =
      dynamic_cast<const __pbase_type_info*>(catch_pointee);
  const auto* thrown_pbase =
      dynamic_cast<const __pbase_type_info*>(thrown_pointee);
  if (catch_pbase && thrown_pbase) {
    return pointer_level_catch(catch_pbase, thrown_pbase, thrown_obj,
                               level + 1, next_prefix);
  }

  if (level == 0) {
    /* Class pointee: the thrown pointee class may derive from the
     * catch pointee class; adjust the stored pointer on success. */
    const auto* catch_class =
        dynamic_cast<const __class_type_info*>(catch_pointee);
    const auto* thrown_class =
        dynamic_cast<const __class_type_info*>(thrown_pointee);
    if (catch_class && thrown_class) {
      void* pointee = *thrown_obj;
      void* adjusted = pointee;
      if (thrown_class->__do_upcast(catch_class, &adjusted)) {
        *thrown_obj = adjusted;
        return true;
      }
    }
  }
  return false;
}
} /* namespace */

bool __pbase_type_info::__do_catch(const std::type_info* thrown_type,
                                   void** thrown_obj, unsigned outer) const {
  if (std::type_info::__do_catch(thrown_type, thrown_obj, outer)) return true;
  const auto* thrown_pbase =
      dynamic_cast<const __pbase_type_info*>(thrown_type);
  if (!thrown_pbase) return false;
  /* A null pointer value is catchable by every pointer handler. */
  if (outer == 0 && thrown_obj && *thrown_obj == nullptr) return true;
  return __pointer_catch(thrown_pbase, thrown_obj, outer);
}

bool __pbase_type_info::__pointer_catch(const __pbase_type_info* thrown_type,
                                        void** thrown_obj,
                                        unsigned outer) const {
  /* The base pbase behavior: same-kind, same-pointee matching
   * through the shared level walker; the derived classes refine
   * the kind checks before delegating here. */
  return pointer_level_catch(this, thrown_type, thrown_obj, outer, true);
}

bool __pointer_type_info::__pointer_catch(
    const __pbase_type_info* thrown_type, void** thrown_obj,
    unsigned outer) const {
  const auto* thrown_pointer =
      dynamic_cast<const __pointer_type_info*>(thrown_type);
  if (!thrown_pointer) return false;
  return pointer_level_catch(this, thrown_pointer, thrown_obj, outer, true);
}

bool __pointer_to_member_type_info::__pointer_catch(
    const __pbase_type_info* thrown_type, void** thrown_obj,
    unsigned outer) const {
  const auto* thrown_member =
      dynamic_cast<const __pointer_to_member_type_info*>(thrown_type);
  if (!thrown_member) return false;
  /* Conservative member-pointer rule (documented in the file
   * header): identical context class and identical member type,
   * with the level-0 qualifier rule on the member type. */
  if (__context != thrown_member->__context) return false;
  if (!(*__pointee == *thrown_member->__pointee)) return false;
  (void)thrown_obj;
  (void)outer;
  return quals_subset(thrown_member->__flags & kQualMask,
                      __flags & kQualMask);
}

} /* namespace __cxxabiv1 */

/* ================================================================
 * Class types: catch matching, the upcast walk, the public-source
 * search, and the dynamic_cast walk.
 *
 * Access quality along a walk is carried as a single __sub_kind
 * value: __contained_public while every step so far was public,
 * __contained_private once any step was not. (The ABI's richer
 * bit combinations collapse to these two for the questions these
 * walks answer: public-and-unambiguous, or not.)
 */

namespace __cxxabiv1 {
namespace {

constexpr int kAccessPublic = __contained_public;
constexpr int kAccessPrivate = __contained_private;

/* The byte offset of a base sub-object inside the object at obj
 * whose dynamic layout the base entry describes. Non-virtual bases
 * carry their offset directly; a virtual base's entry carries the
 * offset, inside the current sub-object's vtable, of the ptrdiff_t
 * holding the base's offset from that sub-object. The vtable is
 * read from the sub-object itself, the reference implementations'
 * discipline: every class in a cast or catch walk that declares
 * virtual bases is polymorphic in the hierarchies this serves. */
std::ptrdiff_t base_subobject_offset(
    const __vmi_class_type_info::__base_class_type_info& base,
    const void* obj) {
  const long off = base.offset();
  if (!base.is_virtual()) return static_cast<std::ptrdiff_t>(off);
  const void* vtable = *static_cast<const void* const*>(obj);
  const char* entry = static_cast<const char*>(vtable) + off;
  return *reinterpret_cast<const std::ptrdiff_t*>(entry);
}

void note_upcast_hit(__class_type_info::__upcast_result& result,
                     const void* obj, int part) {
  if (result.adjustedPtr == nullptr) {
    result.adjustedPtr = const_cast<void*>(obj);
    result.partAccess = static_cast<__sub_kind>(part);
    result.wholeAccess = static_cast<__sub_kind>(part);
    return;
  }
  if (result.adjustedPtr == obj) {
    /* The same sub-object reached again (a shared virtual base):
     * not ambiguity; keep the better access record. */
    if (part == kAccessPublic &&
        result.partAccess != __contained_public) {
      result.partAccess = __contained_public;
      result.wholeAccess = __contained_public;
    }
    return;
  }
  /* A distinct sub-object of the destination type. */
  if (part == kAccessPublic &&
      result.partAccess == __contained_public) {
    result.wholePart = __contained_ambig;
  } else if (part == kAccessPublic) {
    /* A public path supersedes a non-public record of a different
     * sub-object: the public one is the usable answer. */
    result.adjustedPtr = const_cast<void*>(obj);
    result.partAccess = __contained_public;
    result.wholeAccess = __contained_public;
  }
}

void note_dyncast_candidate(__class_type_info::__dyncast_result& result,
                            const void* addr, int access) {
  if (result.dstPtr == nullptr) {
    result.dstPtr = addr;
    result.wholeAccess = static_cast<__sub_kind>(access);
    return;
  }
  if (result.dstPtr == addr) {
    if (access == kAccessPublic) result.wholeAccess = __contained_public;
    return;
  }
  if (access == kAccessPublic &&
      result.wholeAccess == __contained_public) {
    result.wholeAccess = __contained_ambig;
  } else if (access == kAccessPublic) {
    result.dstPtr = addr;
    result.wholeAccess = __contained_public;
  }
}

void note_dyncast_src(__class_type_info::__dyncast_result& result,
                      int access) {
  if (result.wholePart == -1) {
    result.wholePart = access;
  } else if (access == kAccessPublic) {
    result.wholePart = kAccessPublic;
  }
}

/* The completion predicate shared by every __do_dyncast override:
 * a unique public dst candidate and the src sub-object reached
 * through a public path. */
bool dyncast_complete(const __class_type_info::__dyncast_result& result) {
  return result.dstPtr != nullptr &&
         result.wholeAccess == __contained_public &&
         result.wholePart == kAccessPublic;
}

} /* namespace */

bool __class_type_info::__do_catch(const std::type_info* thrown_type,
                                   void** thrown_obj, unsigned outer) const {
  if (std::type_info::__do_catch(thrown_type, thrown_obj, outer)) return true;
  const auto* thrown_class =
      dynamic_cast<const __class_type_info*>(thrown_type);
  if (!thrown_class) return false;
  void* adjusted = *thrown_obj;
  if (thrown_class->__do_upcast(this, &adjusted)) {
    *thrown_obj = adjusted;
    return true;
  }
  return false;
}

bool __class_type_info::__do_upcast(const __class_type_info* dst_type,
                                    void** obj_ptr) const {
  if (this == dst_type) return true;
  __upcast_result result(__contained_public);
  if (!__find_public_base(result, dst_type, *obj_ptr,
                          __contained_public)) {
    return false;
  }
  if (result.wholePart == __contained_ambig) return false;
  if (result.partAccess != __contained_public) return false;
  *obj_ptr = result.adjustedPtr;
  return true;
}

bool __class_type_info::__find_public_base(__upcast_result& result,
                                           const __class_type_info* dst_type,
                                           void* obj_ptr, int part) const {
  if (this == dst_type) {
    note_upcast_hit(result, obj_ptr, part);
    return true;
  }
  return false;
}

__sub_kind __class_type_info::__do_find_public_src(
    std::ptrdiff_t, const void* obj_ptr,
    const __class_type_info* dst_type, const void* src_ptr) const {
  if (this == dst_type && obj_ptr == src_ptr) return __contained_public;
  return __not_contained;
}

bool __class_type_info::__do_dyncast(std::ptrdiff_t, __sub_kind access,
                                     const __class_type_info* dst_type,
                                     const void* obj_ptr,
                                     const __class_type_info* src_type,
                                     const void* src_ptr,
                                     __dyncast_result& result) const {
  if (this == src_type && obj_ptr == src_ptr) {
    note_dyncast_src(result, access);
  }
  if (this == dst_type) {
    note_dyncast_candidate(result, obj_ptr, access);
  }
  return dyncast_complete(result);
}

/* ---- single public non-virtual base ---- */

bool __si_class_type_info::__find_public_base(
    __upcast_result& result, const __class_type_info* dst_type,
    void* obj_ptr, int part) const {
  if (this == dst_type) {
    note_upcast_hit(result, obj_ptr, part);
    return true;
  }
  /* The si encoding is a public base at offset zero: the part
   * quality flows through unchanged. */
  return __base_type->__find_public_base(result, dst_type, obj_ptr, part);
}

__sub_kind __si_class_type_info::__do_find_public_src(
    std::ptrdiff_t src2dst, const void* obj_ptr,
    const __class_type_info* dst_type, const void* src_ptr) const {
  if (this == dst_type && obj_ptr == src_ptr) return __contained_public;
  return __base_type->__do_find_public_src(src2dst, obj_ptr, dst_type,
                                           src_ptr);
}

bool __si_class_type_info::__do_dyncast(
    std::ptrdiff_t src2dst, __sub_kind access,
    const __class_type_info* dst_type, const void* obj_ptr,
    const __class_type_info* src_type, const void* src_ptr,
    __dyncast_result& result) const {
  if (this == src_type && obj_ptr == src_ptr) {
    note_dyncast_src(result, access);
  }
  if (this == dst_type) {
    note_dyncast_candidate(result, obj_ptr, access);
  }
  __base_type->__do_dyncast(src2dst, access, dst_type, obj_ptr, src_type,
                            src_ptr, result);
  return dyncast_complete(result);
}

/* ---- multiple and/or virtual bases ---- */

bool __vmi_class_type_info::__find_public_base(
    __upcast_result& result, const __class_type_info* dst_type,
    void* obj_ptr, int part) const {
  bool found = false;
  if (this == dst_type) {
    note_upcast_hit(result, obj_ptr, part);
    found = true;
  }
  for (unsigned i = 0; i < __base_count; ++i) {
    const __base_class_type_info& base = __base_info[i];
    const std::ptrdiff_t off = base_subobject_offset(base, obj_ptr);
    void* sub = static_cast<char*>(obj_ptr) + off;
    const int sub_part = base.is_public() ? part : kAccessPrivate;
    if (base.__base_type->__find_public_base(result, dst_type, sub,
                                             sub_part)) {
      found = true;
    }
  }
  return found;
}

__sub_kind __vmi_class_type_info::__do_find_public_src(
    std::ptrdiff_t src2dst, const void* obj_ptr,
    const __class_type_info* dst_type, const void* src_ptr) const {
  if (this == dst_type && obj_ptr == src_ptr) return __contained_public;
  for (unsigned i = 0; i < __base_count; ++i) {
    const __base_class_type_info& base = __base_info[i];
    const std::ptrdiff_t off = base_subobject_offset(base, obj_ptr);
    const void* sub = static_cast<const char*>(obj_ptr) + off;
    const __sub_kind inner = base.__base_type->__do_find_public_src(
        src2dst, sub, dst_type, src_ptr);
    if (inner == __not_contained) continue;
    if (inner == __contained_public && base.is_public()) {
      return __contained_public;
    }
    /* Found below, but the path to it is not fully public. The
     * sought address is fixed, so no other base can also hold it:
     * this is the final answer. */
    return __contained_private;
  }
  return __not_contained;
}

bool __vmi_class_type_info::__do_dyncast(
    std::ptrdiff_t src2dst, __sub_kind access,
    const __class_type_info* dst_type, const void* obj_ptr,
    const __class_type_info* src_type, const void* src_ptr,
    __dyncast_result& result) const {
  if (this == src_type && obj_ptr == src_ptr) {
    note_dyncast_src(result, access);
  }
  if (this == dst_type) {
    note_dyncast_candidate(result, obj_ptr, access);
  }
  for (unsigned i = 0; i < __base_count; ++i) {
    const __base_class_type_info& base = __base_info[i];
    const std::ptrdiff_t off = base_subobject_offset(base, obj_ptr);
    const void* sub = static_cast<const char*>(obj_ptr) + off;
    const __sub_kind sub_access =
        (access == __contained_public && base.is_public())
            ? __contained_public
            : __contained_private;
    base.__base_type->__do_dyncast(src2dst, sub_access, dst_type, sub,
                                   src_type, src_ptr, result);
  }
  return dyncast_complete(result);
}

} /* namespace __cxxabiv1 */

/* ================================================================
 * __dynamic_cast: the entry point the compiler calls.
 *
 * Steps, per the ABI: resolve the whole (most-derived) object and
 * its type from the vtable of the object the src pointer addresses;
 * if the destination is the whole type, that is the answer; else
 * run the dyncast walk over the whole object's hierarchy, which
 * succeeds exactly when a unique public dst sub-object exists and
 * the src sub-object sits in the whole object through a public
 * path (this is what makes sibling cross-casts work and ambiguous
 * or inaccessible ones fail); else fall back to the plain upcast
 * within the src sub-object (casting to a base of the static src
 * type that the whole object does not contain separately).
 *
 * src2dst is the compiler's hint about the src sub-object's offset
 * in the whole object (-1 unknown, -2 the cross-cast probe form,
 * >= 0 a known offset). The walk derives everything it needs from
 * the vtable, so the hint is accepted and not needed; the cases it
 * accelerates are small hierarchies here.
 */
extern "C" void* __dynamic_cast(const void* src_ptr,
                                const __cxxabiv1::__class_type_info* src_type,
                                const __cxxabiv1::__class_type_info* dst_type,
                                std::ptrdiff_t src2dst) {
  if (src_ptr == nullptr) return nullptr;
  const void* const* vtable =
      *static_cast<const void* const* const*>(src_ptr);
  const std::ptrdiff_t offset_to_top =
      reinterpret_cast<const std::ptrdiff_t*>(vtable)[-2];
  const auto* whole_type =
      reinterpret_cast<const __cxxabiv1::__class_type_info*>(vtable[-1]);
  const void* whole_ptr = static_cast<const char*>(src_ptr) + offset_to_top;

  if (whole_type == dst_type) {
    return const_cast<void*>(whole_ptr);
  }

  __cxxabiv1::__class_type_info::__dyncast_result result;
  result.wholePtr = whole_ptr;
  whole_type->__do_dyncast(src2dst, __cxxabiv1::__contained_public,
                           dst_type, whole_ptr, src_type, src_ptr, result);
  if (result.wholeAccess == __cxxabiv1::__contained_public &&
      result.wholePart == __cxxabiv1::__contained_public) {
    return const_cast<void*>(result.dstPtr);
  }

  /* No usable whole-object answer: the destination is absent from
   * the whole object, present only through a non-public path, or
   * ambiguous in the dynamic type (two distinct public candidates:
   * the wholeAccess record reads __contained_ambig). The remaining
   * legal answer is the plain upcast within the src sub-object:
   * the standard's clause order gives a public base of the STATIC
   * src type its own route to success, independent of the dynamic
   * type's shape. (Verified against the host runtime in the test
   * pass: casting a branch pointer to its own branch's base
   * succeeds even when that base is ambiguous in the whole object,
   * while the same base cast from an unrelated branch fails.) */
  void* obj = const_cast<void*>(src_ptr);
  if (src_type->__do_upcast(dst_type, &obj)) return obj;
  return nullptr;
}

namespace __cxxabiv1 {

/* ================================================================
 * The fundamental typeinfo objects (_ZTIv, _ZTIb, _ZTIi, and the
 * rest) are intentionally NOT spelled out in this file. The ABI's
 * emission rule anchors them to the translation unit that defines
 * __fundamental_type_info's key function: the compiler emits the
 * whole fundamental set there as vague-linkage definitions,
 * alongside the class's vtable. That destructor is defined at the
 * top of this file, which makes this the anchor TU; hand-writing
 * the objects here as well (tried, with asm labels pinning the
 * symbols) collides with the compiler's own emission in the same
 * TU. The host test pass verifies with nm that this object carries
 * the full set: v, b, w, c, a, h, s, t, i, j, l, m, x, y, f, d, e,
 * Du (char8_t), Ds (char16_t), Di (char32_t), Dn (nullptr_t).
 * The toolchain piece confirms clang follows the same anchor rule
 * on wasm32; it is the standard mechanism the reference runtimes
 * rely on.
 */

} /* namespace __cxxabiv1 */
