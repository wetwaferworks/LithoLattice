/* std_exceptions.cc ... the std::exception family the ABI owns,
 * the handler variables, and the terminate plumbing.
 *
 * Ownership, following the reference ABIs: the exception classes
 * whose names appear in every C++ binary's ABI traffic
 * (std::exception, std::bad_exception, std::bad_alloc,
 * std::bad_array_new_length, std::bad_cast, std::bad_typeid) have
 * their vtables and typeinfos anchored here, in the runtime, not in
 * the stdlib piece. The future stdlib headers declare these same
 * classes (the seeded headers in include/ are already that shape)
 * and the stdlib's own exception types (runtime_error and family)
 * build on them from its side.
 */

#include <exception>
#include <new>
#include <typeinfo>

#include "lithoabi_internal.h"

namespace std {

exception::~exception() = default;
const char* exception::what() const noexcept { return "std::exception"; }

bad_exception::~bad_exception() = default;
const char* bad_exception::what() const noexcept {
  return "std::bad_exception";
}

bad_alloc::~bad_alloc() = default;
const char* bad_alloc::what() const noexcept { return "std::bad_alloc"; }

bad_array_new_length::~bad_array_new_length() = default;
const char* bad_array_new_length::what() const noexcept {
  return "std::bad_array_new_length";
}

bad_cast::~bad_cast() = default;
const char* bad_cast::what() const noexcept { return "std::bad_cast"; }

bad_typeid::~bad_typeid() = default;
const char* bad_typeid::what() const noexcept { return "std::bad_typeid"; }

const nothrow_t nothrow{};

} /* namespace std */

namespace __cxxabiv1 {

/* The default terminate behavior: say what happened through the
 * state-free console sink, then end the instance through LithoSys.
 * No allocation, no locks, no libc state: this runs when the world
 * is already broken. */
static void lithoabi_default_terminate() {
  lithoabi_diag("lithoabi: terminate called\n");
  lithosys_abort();
}
static void lithoabi_default_unexpected() { std::terminate(); }

std::terminate_handler __cxa_terminate_handler = &lithoabi_default_terminate;
std::unexpected_handler __cxa_unexpected_handler = &lithoabi_default_unexpected;
std::new_handler __cxa_new_handler = nullptr;

[[noreturn]] void lithoabi_terminate_now(const char* reason) {
  if (reason) lithoabi_diag(reason);
  std::terminate();
}

} /* namespace __cxxabiv1 */

namespace std {

terminate_handler set_terminate(terminate_handler handler) noexcept {
  terminate_handler old = __cxxabiv1::__cxa_terminate_handler;
  if (handler) __cxxabiv1::__cxa_terminate_handler = handler;
  return old;
}
unexpected_handler set_unexpected(unexpected_handler handler) noexcept {
  unexpected_handler old = __cxxabiv1::__cxa_unexpected_handler;
  if (handler) __cxxabiv1::__cxa_unexpected_handler = handler;
  return old;
}
terminate_handler get_terminate() noexcept {
  return __cxxabiv1::__cxa_terminate_handler;
}
unexpected_handler get_unexpected() noexcept {
  return __cxxabiv1::__cxa_unexpected_handler;
}

[[noreturn]] void terminate() noexcept {
  terminate_handler handler = __cxxabiv1::__cxa_terminate_handler;
  if (handler) handler();
  /* A terminate handler that returns leaves the process nowhere
   * legal to go. */
  lithosys_abort();
}

[[noreturn]] void unexpected() {
  unexpected_handler handler = __cxxabiv1::__cxa_unexpected_handler;
  if (handler) handler();
  terminate();
}

bool uncaught_exception() noexcept { return uncaught_exceptions() > 0; }
int uncaught_exceptions() noexcept {
  __cxxabiv1::__cxa_eh_globals* globals =
      __cxxabiv1::__cxa_get_globals_fast();
  return globals ? static_cast<int>(globals->uncaughtExceptions) : 0;
}

} /* namespace std */
