/* typeinfo_layout.h ... the __cxxabiv1 type_info classes (internal).
 *
 * The class definitions the compiler's emitted typeinfo objects are
 * built against. Object layouts and vtable slot orders are Itanium
 * ABI data layout: std::type_info contributes (vtable, name); each
 * derived class appends exactly the fields declared here, and the
 * virtuals appear in the order the reference ABIs declare them.
 * LithoAbi's own TUs and the future toolchain build share this one
 * definition, which is what makes catch matching and dynamic_cast
 * agree across the module.
 *
 * Only LithoAbi's implementation files and its tests include this
 * header. The public surface (include/cxxabi.h) forward-declares
 * __class_type_info and nothing more.
 */
#ifndef LITHOABI_TYPEINFO_LAYOUT_H
#define LITHOABI_TYPEINFO_LAYOUT_H

#include <typeinfo>
#include <cstddef>

namespace __cxxabiv1 {

/* Sub-object search outcome codes, shared by the upcast and
 * dynamic_cast walks (__do_catch consumes them too). The values are
 * the ABI's: a bit field describing how the destination sub-object
 * was found inside the source object. */
enum __sub_kind {
  __contained_public = 0,
  __not_contained = 1,
  __contained_ambig = 2,
  __contained_mask = 4,        /* bit: the whole-object path was masked */
  __contained_virtual_mask = 8,
  __contained_private = 0x10,
  __contained_public_mask = 0x20
};

class __fundamental_type_info : public std::type_info {
public:
  ~__fundamental_type_info() override;
  explicit __fundamental_type_info(const char* name) : std::type_info(name) {}
};

class __array_type_info : public std::type_info {
public:
  ~__array_type_info() override;
  explicit __array_type_info(const char* name) : std::type_info(name) {}
};

class __enum_type_info : public std::type_info {
public:
  ~__enum_type_info() override;
  explicit __enum_type_info(const char* name) : std::type_info(name) {}
};

class __function_type_info : public std::type_info {
public:
  ~__function_type_info() override;
  explicit __function_type_info(const char* name) : std::type_info(name) {}

protected:
  bool __is_function_p() const override;
};

/* Base for the pointer-like types: a flags word (the cv and
 * access qualifier masks of the pointee) and the pointee type. */
class __pbase_type_info : public std::type_info {
public:
  ~__pbase_type_info() override;

  /* The flags word's bit allocation in the encoding the compiler
   * emits (verified against host g++ typeinfo emission in the
   * LithoAbi test pass: const int* carries 0x1, volatile int*
   * carries 0x2, const volatile int* carries 0x3, and each pointer
   * level describes its own pointee). Only the three cv bits take
   * part in catch matching's qualification rules. */
  enum __masks {
    __const_mask = 0x1,
    __volatile_mask = 0x2,
    __restrict_mask = 0x4,
    __incomplete_mask = 0x8,
    __incomplete_class_mask = 0x10,
    __transaction_safe_mask = 0x20,
    __noexcept_mask = 0x40
  };

  unsigned int __flags;
  const std::type_info* __pointee;

  __pbase_type_info(const char* name, int flags,
                    const std::type_info* pointee)
      : std::type_info(name), __flags(flags), __pointee(pointee) {}

protected:
  bool __do_catch(const std::type_info* thrown_type, void** thrown_obj,
                  unsigned outer) const override;
  /* Catch matching for pointer types, once both sides are known to
   * be pbase types. outer counts enclosing pointer levels; the
   * qualification rules are applied level by level in the standard
   * multi-level form (see typeinfo.cc). */
  virtual bool __pointer_catch(const __pbase_type_info* thrown_type,
                               void** thrown_obj, unsigned outer) const;
};

class __pointer_type_info : public __pbase_type_info {
public:
  ~__pointer_type_info() override;
  __pointer_type_info(const char* name, int flags,
                      const std::type_info* pointee)
      : __pbase_type_info(name, flags, pointee) {}

protected:
  bool __is_pointer_p() const override;
  bool __pointer_catch(const __pbase_type_info* thrown_type,
                       void** thrown_obj, unsigned outer) const override;
};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
  ~__pointer_to_member_type_info() override;

  const __class_type_info* __context;

  __pointer_to_member_type_info(const char* name, int flags,
                                const std::type_info* pointee,
                                const __class_type_info* context)
      : __pbase_type_info(name, flags, pointee), __context(context) {}

protected:
  bool __pointer_catch(const __pbase_type_info* thrown_type,
                       void** thrown_obj, unsigned outer) const override;
};

class __class_type_info : public std::type_info {
public:
  ~__class_type_info() override;
  explicit __class_type_info(const char* name) : std::type_info(name) {}

  /* Result records for the two walks, in the ABI's shapes. */
  struct __upcast_result {
    void* adjustedPtr;
    __sub_kind wholeAccess;    /* access of the path to the whole object */
    int wholePart;             /* __contained_* for the whole object */
    __sub_kind partAccess;     /* access of the final step */
    __upcast_result(int part) : adjustedPtr(nullptr), wholeAccess(__contained_public), wholePart(part), partAccess(__contained_public) {}
  };
  struct __dyncast_result {
    void* adjustedPtr;
    __sub_kind wholeAccess;
    int wholePart;
    __sub_kind dst2srcAccess;  /* access of the dst sub-object in the src */
    const void* dstPtr;        /* address of the found dst sub-object */
    const void* wholePtr;      /* address of the whole (dynamic) object */
    int wholeType;             /* unused by this implementation */
    /* Field use in this implementation (the record's shape is the
     * ABI's; the state discipline is ours): dstPtr == nullptr means
     * no dst candidate yet, and wholeAccess then reads
     * __not_contained; with a candidate, wholeAccess is its access
     * quality (__contained_public, __contained_private, or
     * __contained_ambig once a second distinct public candidate was
     * seen). wholePart tracks the src sub-object: -1 until the walk
     * reaches it, then the access quality it was reached with.
     * dst2srcAccess is not maintained; the publicness checks it
     * would refine are folded into wholeAccess and wholePart. */
    __dyncast_result() : adjustedPtr(nullptr), wholeAccess(__not_contained), wholePart(-1), dst2srcAccess(__not_contained), dstPtr(nullptr), wholePtr(nullptr), wholeType(0) {}
  };

public:
  /* Public for the same reason as std::type_info's hooks (see
   * include/typeinfo): the walks call each other through base
   * pointers from sibling classes and free functions. Access
   * control does not affect the vtable layout. */
  bool __do_catch(const std::type_info* thrown_type, void** thrown_obj,
                  unsigned outer) const override;
  bool __do_upcast(const __class_type_info* dst_type,
                   void** obj_ptr) const override;

  /* The dynamic_cast walk and the public-base search, as virtuals so
   * the si and vmi layouts extend them with their base lists. Slot
   * order after the type_info virtuals is ABI-fixed: dyncast first,
   * then the public-source search. */
  virtual bool __do_dyncast(std::ptrdiff_t src2dst,
                            __sub_kind src2dst_access,
                            const __class_type_info* dst_type,
                            const void* obj_ptr,
                            const __class_type_info* src_type,
                            const void* src_ptr,
                            __dyncast_result& result) const;
  virtual __sub_kind __do_find_public_src(std::ptrdiff_t src2dst,
                                          const void* obj_ptr,
                                          const __class_type_info* dst_type,
                                          const void* src_ptr) const;
  /* The upcast walk's recursive engine: search this class (as the
   * current sub-object type at obj_ptr) for a dst_type sub-object. */
  virtual bool __find_public_base(__upcast_result& result,
                                  const __class_type_info* dst_type,
                                  void* obj_ptr, int part) const;
};

class __si_class_type_info : public __class_type_info {
public:
  ~__si_class_type_info() override;

  const __class_type_info* __base_type;

  __si_class_type_info(const char* name, const __class_type_info* base)
      : __class_type_info(name), __base_type(base) {}

public:
  bool __do_dyncast(std::ptrdiff_t src2dst, __sub_kind src2dst_access,
                    const __class_type_info* dst_type, const void* obj_ptr,
                    const __class_type_info* src_type, const void* src_ptr,
                    __dyncast_result& result) const override;
  __sub_kind __do_find_public_src(std::ptrdiff_t src2dst,
                                  const void* obj_ptr,
                                  const __class_type_info* dst_type,
                                  const void* src_ptr) const override;
  bool __find_public_base(__upcast_result& result,
                          const __class_type_info* dst_type, void* obj_ptr,
                          int part) const override;
};

class __vmi_class_type_info : public __class_type_info {
public:
  ~__vmi_class_type_info() override;

  enum __flags_masks {
    __non_diamond_repeat_mask = 0x1,
    __diamond_shaped_mask = 0x2,
    __flags_unknown_mask = 0x10
  };
  struct __base_class_type_info {
    const __class_type_info* __base_type;
    long __offset_flags;
    /* Bit allocation as the compiler emits it, verified against
     * host g++ typeinfo emission in the LithoAbi test pass: a
     * public non-virtual base carries 0x2 in the low byte, a
     * virtual base adds 0x1 (and stores a negative vtable-relative
     * offset), a private base carries neither bit. */
    enum __offset_flags_masks {
      __virtual_mask = 0x1,
      __public_mask = 0x2,
      __offset_shift = 8
    };
    long offset() const {
      return __offset_flags >> __offset_shift;
    }
    bool is_public() const { return __offset_flags & __public_mask; }
    bool is_virtual() const { return __offset_flags & __virtual_mask; }
  };

  unsigned int __flags;
  unsigned int __base_count;
  __base_class_type_info __base_info[1];

  __vmi_class_type_info(const char* name, int flags)
      : __class_type_info(name), __flags(flags), __base_count(0) {}

public:
  bool __do_dyncast(std::ptrdiff_t src2dst, __sub_kind src2dst_access,
                    const __class_type_info* dst_type, const void* obj_ptr,
                    const __class_type_info* src_type, const void* src_ptr,
                    __dyncast_result& result) const override;
  __sub_kind __do_find_public_src(std::ptrdiff_t src2dst,
                                  const void* obj_ptr,
                                  const __class_type_info* dst_type,
                                  const void* src_ptr) const override;
  bool __find_public_base(__upcast_result& result,
                          const __class_type_info* dst_type, void* obj_ptr,
                          int part) const override;
};

} /* namespace __cxxabiv1 */

#endif /* LITHOABI_TYPEINFO_LAYOUT_H */
