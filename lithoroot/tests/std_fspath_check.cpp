/* filesystem::path differential check (S4 tail): this file builds for wasm
 * (LithoRoot) and for the host (libstdc++); outputs must match exactly. */
#include <filesystem>
#include <string>
#include <cstddef>
#ifdef __wasm__
static unsigned char g_arena[16 << 20];
static size_t g_top;
void* operator new(size_t n) { n = (n + 15) & ~size_t(15); void* p = g_arena + g_top; g_top += n; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
#endif
#include "std_fspath_cases.inc"
using P = std::filesystem::path;
static std::string S(const P& p) { return p.string(); }
static std::string out;
extern "C" const char* fspath_dump() {
  out.clear();
#include "std_fspath_dump.inc"
  return out.c_str();
}
extern "C" unsigned fspath_len() { return unsigned(out.size()); }
#ifndef __wasm__
#include <cstdio>
int main() { fputs(fspath_dump(), stdout); }
#endif
