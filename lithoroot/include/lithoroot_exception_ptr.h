/* lithoroot_exception_ptr.h ... LithoRoot std L7 (S4 tail, 2026-11-16): the
 * stdlib half of <exception> that header's note reserves for the stdlib
 * piece: exception_ptr, current_exception, rethrow_exception,
 * make_exception_ptr, nested_exception, throw_with_nested,
 * rethrow_if_nested. Built only on LithoAbi's primary-exception refcount
 * calls (cxxabi.h); the refcount is atomic in LithoAbi, so an exception_ptr
 * may cross threads (census: sim_thread, software_renderer, main.cpp). */
#ifndef LITHOROOT_EXCEPTION_PTR_H
#define LITHOROOT_EXCEPTION_PTR_H

namespace __cxxabiv1 {
struct __cxa_exception;
extern "C" {
__cxa_exception* __cxa_current_primary_exception();
void __cxa_rethrow_primary_exception(__cxa_exception* primary);
void __cxa_increment_exception_refcount(__cxa_exception* primary);
void __cxa_decrement_exception_refcount(__cxa_exception* primary);
}
}  // namespace __cxxabiv1

namespace std {

class exception_ptr;
exception_ptr current_exception() noexcept;
[[noreturn]] void rethrow_exception(exception_ptr p);

class exception_ptr {
 public:
  exception_ptr() noexcept = default;
  exception_ptr(decltype(nullptr)) noexcept {}
  exception_ptr(const exception_ptr& o) noexcept : p_(o.p_) { __cxxabiv1::__cxa_increment_exception_refcount(p_); }
  exception_ptr(exception_ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  exception_ptr& operator=(const exception_ptr& o) noexcept {
    __cxxabiv1::__cxa_increment_exception_refcount(o.p_);
    __cxxabiv1::__cxa_decrement_exception_refcount(p_);
    p_ = o.p_;
    return *this;
  }
  exception_ptr& operator=(exception_ptr&& o) noexcept {
    if (this != &o) { __cxxabiv1::__cxa_decrement_exception_refcount(p_); p_ = o.p_; o.p_ = nullptr; }
    return *this;
  }
  ~exception_ptr() { __cxxabiv1::__cxa_decrement_exception_refcount(p_); }
  explicit operator bool() const noexcept { return p_ != nullptr; }
  friend bool operator==(const exception_ptr& a, const exception_ptr& b) noexcept { return a.p_ == b.p_; }
  friend void swap(exception_ptr& a, exception_ptr& b) noexcept { auto t = a.p_; a.p_ = b.p_; b.p_ = t; }

 private:
  friend exception_ptr current_exception() noexcept;
  friend void rethrow_exception(exception_ptr);
  __cxxabiv1::__cxa_exception* p_ = nullptr;  // owns one reference
};

/* The exception being handled on this thread (refcount taken by LithoAbi), or null. */
inline exception_ptr current_exception() noexcept {
  exception_ptr r;
  r.p_ = __cxxabiv1::__cxa_current_primary_exception();
  return r;
}
inline void rethrow_exception(exception_ptr p) {
  if (!p.p_) terminate();  // the standard makes a null rethrow undefined; fail loud
  __cxxabiv1::__cxa_rethrow_primary_exception(p.p_);
  __builtin_unreachable();
}
template <class E> exception_ptr make_exception_ptr(E e) noexcept {
  try { throw e; } catch (...) { return current_exception(); }
}

class nested_exception {
 public:
  nested_exception() noexcept : p_(current_exception()) {}
  nested_exception(const nested_exception&) noexcept = default;
  nested_exception& operator=(const nested_exception&) noexcept = default;
  virtual ~nested_exception() = default;
  [[noreturn]] void rethrow_nested() const { if (!p_) terminate(); rethrow_exception(p_); }
  exception_ptr nested_ptr() const noexcept { return p_; }
 private:
  exception_ptr p_;
};

template <class T> [[noreturn]] void throw_with_nested(T&& t) {
  using U = __remove_cvref(T);
  if constexpr (__is_class(U) && !__is_final(U) && !__is_base_of(nested_exception, U)) {
    struct both : U, nested_exception { explicit both(T&& x) : U(static_cast<T&&>(x)) {} };
    throw both(static_cast<T&&>(t));
  } else {
    throw static_cast<T&&>(t);
  }
}
template <class E> void rethrow_if_nested(const E& e) {
  if constexpr (__is_polymorphic(E) && (!__is_base_of(nested_exception, E) || __is_convertible_to(E*, nested_exception*))) {
    if (auto* n = dynamic_cast<const nested_exception*>(&e)) n->rethrow_nested();
  }
}

}  // namespace std
#endif
