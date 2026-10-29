/* stubs.cc ... the pure-virtual and deleted-virtual stubs.
 *
 * A call through a pure virtual slot, or to a deleted virtual
 * function, is a program error with no legal continuation. Both
 * stubs say which one fired through the state-free console sink and
 * end in the terminate path, which is the behavior the reference
 * ABIs implement and the one a debugger wants: a named diagnostic
 * at the exact point of failure, then the standard end.
 */

#include "lithoabi_internal.h"

namespace __cxxabiv1 {

[[noreturn]] void __cxa_pure_virtual() {
  lithoabi_terminate_now("lithoabi: pure virtual function called\n");
}

[[noreturn]] void __cxa_deleted_virtual() {
  lithoabi_terminate_now("lithoabi: deleted virtual function called\n");
}

} /* namespace __cxxabiv1 */
