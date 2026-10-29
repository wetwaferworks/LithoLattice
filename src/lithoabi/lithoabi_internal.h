/* lithoabi_internal.h ... LithoAbi cross-file internals.
 *
 * The exception header layout, the conversion helpers every EH file
 * uses, the eh globals access, and the console diagnostic helper.
 * Nothing here is public surface: the compiler-facing types live in
 * include/cxxabi.h, and the target-specific primitives live in
 * lithoabi_wasm_eh.h.
 */
#ifndef LITHOABI_INTERNAL_H
#define LITHOABI_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lithosys.h"
#include "cxxabi.h"
#include "lithoabi_atomic.h"

namespace __cxxabiv1 {

/* The unwind header, in the Itanium shape. It sits inside the
 * exception header so that &unwindHeader is the pointer the
 * unwinder and the landing pads traffic in. exception_class is the
 * Itanium C++ class value ("GNUCC++\0" read as a little-endian
 * uint64) so a foreign-unwinder or a debugger can recognize ours. */
enum _Unwind_Reason_Code {
  _URC_NO_REASON = 0,
  _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
  _URC_END_OF_STACK = 5,
  _URC_HANDLER_FOUND = 6,
  _URC_INSTALL_CONTEXT = 7,
  _URC_CONTINUE_UNWIND = 8,
  _URC_FAILURE = 9
};
typedef int _Unwind_Action;
enum {
  _UA_SEARCH_PHASE = 1,
  _UA_CLEANUP_PHASE = 2,
  _UA_HANDLER_FRAME = 4,
  _UA_FORCE_UNWIND = 8,
  _UA_END_OF_STACK = 16
};
struct _Unwind_Exception {
  uint64_t exception_class;
  void (*exception_cleanup)(_Unwind_Reason_Code, _Unwind_Exception*);
  uintptr_t private_1;
  uintptr_t private_2;
};
inline uint64_t lithoabi_cpp_exception_class() {
  /* 'G','N','U','C','C','+','+',0 packed little-endian. */
  return 0x00432b2b4343554e47ull;
}

/* The exception header. The first member is the reference count of
 * the refcounted layout (the layout the primary-exception family
 * requires: an exception that std::exception_ptr can hold must be
 * able to outlive its catch, so every allocation carries the
 * count). The Itanium __cxa_exception fields follow in their fixed
 * order, the unwind header among them. Two LithoAbi fields trail
 * the unwind header (flags and primaryObject): they exist for
 * __cxa_init_primary_exception, whose object lives in a separate
 * allocation. Their position is safe because the only offset any
 * other component relies on is "object follows the whole header",
 * and every conversion in this library goes through the helpers
 * below rather than open-coded arithmetic.
 *
 * Object pointer discipline:
 *   allocate: object = (char*)header + sizeof(lithoabi_eh_header),
 *             aligned up to LITHOABI_EH_ALIGNMENT.
 *   header_of_object(obj) = (lithoabi_eh_header*)((char*)obj - header_size)
 *   header_of_unwind(ue)  = container_of(ue, unwindHeader)
 * For a primary exception built by __cxa_init_primary_exception the
 * header block is header-only, flags carries EXTERNAL_OBJECT, and
 * primaryObject is the object; header_of_object is never applied to
 * it (its users hold the header directly). */
constexpr std::size_t LITHOABI_EH_ALIGNMENT = 16;
struct lithoabi_eh_header {
  uint32_t referenceCount;          /* atomic; starts at 1 */
  uint32_t flags;
  std::type_info* exceptionType;
  __cxa_destructor_fn exceptionDestructor;
  std::unexpected_handler unexpectedHandler;
  std::terminate_handler terminateHandler;
  lithoabi_eh_header* nextException;
  int handlerCount;
  int handlerSwitchValue;
  const char* actionRecord;
  const char* languageSpecificData;
  void* catchTemp;
  void* adjustedPtr;
  _Unwind_Exception unwindHeader;
  void* primaryObject;              /* only for EXTERNAL_OBJECT */
};
enum {
  LITHOABI_EH_FLAG_FROM_POOL = 1,      /* storage is an emergency slot */
  LITHOABI_EH_FLAG_EXTERNAL_OBJECT = 2 /* object not after this header */
};

constexpr std::size_t lithoabi_header_size() {
  return (sizeof(lithoabi_eh_header) + (LITHOABI_EH_ALIGNMENT - 1)) &
         ~(LITHOABI_EH_ALIGNMENT - 1);
}
inline lithoabi_eh_header* lithoabi_header_of_object(void* obj) {
  return reinterpret_cast<lithoabi_eh_header*>(
      static_cast<char*>(obj) - lithoabi_header_size());
}
inline void* lithoabi_object_of_header(lithoabi_eh_header* header) {
  if (header->flags & LITHOABI_EH_FLAG_EXTERNAL_OBJECT)
    return header->primaryObject;
  return reinterpret_cast<char*>(header) + lithoabi_header_size();
}
inline lithoabi_eh_header* lithoabi_header_of_unwind(_Unwind_Exception* ue) {
  return reinterpret_cast<lithoabi_eh_header*>(
      reinterpret_cast<char*>(ue) -
      offsetof(lithoabi_eh_header, unwindHeader));
}
inline _Unwind_Exception* lithoabi_unwind_of_header(lithoabi_eh_header* h) {
  return &h->unwindHeader;
}

/* Terminate plumbing shared by the EH files (std_exceptions.cc). */
[[noreturn]] void lithoabi_terminate_now(const char* reason);

/* Cross-file helpers from exception.cc: the single release point
 * for exception lifetime, and the allocation entry that marks the
 * caller as the bad_alloc path (for the emergency pool's reserved
 * slot discipline). */
bool lithoabi_maybe_release(lithoabi_eh_header* header);
void* lithoabi_allocate_exception_for_bad_alloc(std::size_t thrown_size);

/* The runtime's own bad_alloc thrower (eh_runtime.cc): the one
 * disciplined consumer of the emergency pool's reserved slot. */
[[noreturn]] void lithoabi_throw_bad_alloc();

/* Console diagnostic through the LithoSys last-resort sink: safe
 * from any context this library runs in, including inside abort
 * paths. Two-part and three-part forms keep call sites terse. */
inline void lithoabi_diag(const char* a) {
  lithosys_console_write(a, static_cast<uint32_t>(std::strlen(a)));
}
inline void lithoabi_diag(const char* a, const char* b) {
  lithoabi_diag(a);
  lithoabi_diag(b);
}
inline void lithoabi_diag(const char* a, const char* b, const char* c) {
  lithoabi_diag(a);
  lithoabi_diag(b);
  lithoabi_diag(c);
}

} /* namespace __cxxabiv1 */

#endif /* LITHOABI_INTERNAL_H */
