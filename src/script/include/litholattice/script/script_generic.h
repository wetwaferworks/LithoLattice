// Fix 3c (JD 2026-10-23, Elfie P2): generic-calling-convention wrappers for script bindings.
//
// AngelScript built with AS_MAX_PORTABILITY (every WASM build) has no native calling conventions:
// registering asCALL_CDECL / asCALL_CDECL_OBJLAST returns asNOT_SUPPORTED (-7). These templates
// turn a plain C++ function into an asCALL_GENERIC thunk, so each binding carries both forms and
// ScriptEngine::bindFunction() picks one (native unless the library is portable or the engine is
// forced generic for testing). ObjLast<&f>: the last C++ parameter is the auxiliary object passed
// to bindFunction (asCALL_CDECL_OBJLAST); Cdecl<&f>: no auxiliary. Supported argument / return
// types: std::string (by value or const&), bool, float, double, 8/16/32/64-bit integers, void.
#pragma once
#include <angelscript.h>
#include <cstddef>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include "litholattice/script/script_engine.h"

namespace litholattice::scriptgen {

template <class T>
decltype(auto) arg(asIScriptGeneric* g, asUINT i) {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  if constexpr (std::is_same_v<U, std::string>) return static_cast<const std::string&>(*static_cast<std::string*>(g->GetArgObject(i)));
  else if constexpr (std::is_same_v<U, bool>) return bool(g->GetArgByte(i) != 0);
  else if constexpr (std::is_same_v<U, float>) return float(g->GetArgFloat(i));
  else if constexpr (std::is_same_v<U, double>) return double(g->GetArgDouble(i));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 8) return U(g->GetArgQWord(i));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 4) return U(g->GetArgDWord(i));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 2) return U(g->GetArgWord(i));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 1) return U(g->GetArgByte(i));
  else static_assert(sizeof(U) == 0, "scriptgen: unsupported argument type");
}

template <class R>
void ret(asIScriptGeneric* g, R&& r) {
  using U = std::remove_cv_t<std::remove_reference_t<R>>;
  if constexpr (std::is_same_v<U, std::string>) new (g->GetAddressOfReturnLocation()) std::string(std::forward<R>(r));
  else if constexpr (std::is_same_v<U, bool>) g->SetReturnByte(r ? 1 : 0);
  else if constexpr (std::is_same_v<U, float>) g->SetReturnFloat(r);
  else if constexpr (std::is_same_v<U, double>) g->SetReturnDouble(r);
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 8) g->SetReturnQWord(asQWORD(r));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 4) g->SetReturnDWord(asDWORD(r));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 2) g->SetReturnWord(asWORD(r));
  else if constexpr (std::is_integral_v<U> && sizeof(U) == 1) g->SetReturnByte(asBYTE(r));
  else static_assert(sizeof(U) == 0, "scriptgen: unsupported return type");
}

template <auto F> struct ObjLast;
template <class R, class... P, R (*F)(P...)>
struct ObjLast<F> {
  static_assert(sizeof...(P) >= 1, "ObjLast: the function needs the object as its last parameter");
  static void call(asIScriptGeneric* g) { impl(g, std::make_index_sequence<sizeof...(P) - 1>{}); }

 private:
  using Tup = std::tuple<P...>;
  template <std::size_t... I>
  static void impl(asIScriptGeneric* g, std::index_sequence<I...>) {
    using Obj = std::tuple_element_t<sizeof...(P) - 1, Tup>;
    Obj obj = static_cast<Obj>(g->GetAuxiliary());
    if constexpr (std::is_void_v<R>) F(arg<std::tuple_element_t<I, Tup>>(g, asUINT(I))..., obj);
    else ret(g, F(arg<std::tuple_element_t<I, Tup>>(g, asUINT(I))..., obj));
  }
};

template <auto F> struct Cdecl;
template <class R, class... P, R (*F)(P...)>
struct Cdecl<F> {
  static void call(asIScriptGeneric* g) { impl(g, std::make_index_sequence<sizeof...(P)>{}); }

 private:
  using Tup = std::tuple<P...>;
  template <std::size_t... I>
  static void impl(asIScriptGeneric* g, std::index_sequence<I...>) {
    (void)g;
    if constexpr (std::is_void_v<R>) F(arg<std::tuple_element_t<I, Tup>>(g, asUINT(I))...);
    else ret(g, F(arg<std::tuple_element_t<I, Tup>>(g, asUINT(I))...));
  }
};

}  // namespace litholattice::scriptgen

/// Both forms of a binding whose last C++ parameter is the auxiliary object.
#define LL_SCRIPT_FN_OBJLAST(f) \
  (::litholattice::ScriptEngine::ScriptFn{asFUNCTION(f), asFUNCTION((::litholattice::scriptgen::ObjLast<&f>::call)), asCALL_CDECL_OBJLAST})
/// Both forms of a plain cdecl binding.
#define LL_SCRIPT_FN_CDECL(f) \
  (::litholattice::ScriptEngine::ScriptFn{asFUNCTION(f), asFUNCTION((::litholattice::scriptgen::Cdecl<&f>::call)), asCALL_CDECL})
