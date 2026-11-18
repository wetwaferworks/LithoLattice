// new_handler_check.cpp ... std::set_new_handler / std::get_new_handler in LithoAbi, and the weak
// operator new/delete in LithoLibC.
//
// This test REPLACES operator new(size_t) and operator delete(void*) the way LithoKraft's
// script_memory_hook.cpp does; the link must succeed (LithoLibC's forms are weak) and the
// replacements must be the ones called. The handler accessors must round-trip. main() returns the
// number of failures.
#include <cstdio>
#include <cstdlib>
#include <new>

static int g_news, g_deletes, fails;
void* operator new(std::size_t n) { ++g_news; return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { ++g_deletes; std::free(p); }

static int* volatile g_keep;  // the pointers escape, so new/delete pairs are not elided
static void handler_a() {}
static void handler_b() {}
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s\n", #c); ++fails; } } while (0)

int main() {
  CHECK(std::get_new_handler() == nullptr);
  CHECK(std::set_new_handler(handler_a) == nullptr);
  CHECK(std::get_new_handler() == handler_a);
  CHECK(std::set_new_handler(handler_b) == handler_a);
  CHECK(std::set_new_handler(nullptr) == handler_b);
  CHECK(std::get_new_handler() == nullptr);
  g_keep = new int(7);
  CHECK(g_news == 1 && *g_keep == 7);
  delete g_keep;  // the compiler may pick the sized form; it must reach the replacement
  CHECK(g_deletes == 1);
  // Not replaced here: LithoLibC's weak forms serve these and forward to the replaced base forms.
  g_keep = new (std::nothrow) int(3);
  CHECK(g_keep != nullptr && *g_keep == 3 && g_news == 2);
  ::operator delete(g_keep, std::nothrow);
  CHECK(g_deletes == 2);
  g_keep = new int[4];
  CHECK(g_news == 3);
  delete[] g_keep;
  CHECK(g_deletes == 3);
  std::printf("new_handler_check: %d failure(s)\n", fails);
  return fails;
}
