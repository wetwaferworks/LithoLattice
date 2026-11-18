/* streams subset checks (S5 fstream/sstream). Node, -O2 and -O0. Returns 0 on pass.
 * stringstream runs here; fstream is compile-checked with the engine's idioms (its
 * I/O needs the LithoVFS stdio seam, S7). */
#include <sstream>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

static unsigned char g_arena[4 << 20];
static size_t g_top;
void* operator new(size_t n) { n = (n + 15) & ~size_t(15); void* p = g_arena + g_top; g_top += n; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void*) noexcept {}
void operator delete[](void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
void operator delete[](void*, size_t) noexcept {}

struct FakePath { std::string s; const char* c_str() const { return s.c_str(); } };

// Engine idioms, compile only.
[[maybe_unused]] static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
[[maybe_unused]] static void idioms(const FakePath& dir, const char* d, size_t n) {
  std::ofstream(dir) << "x" << 3 << '\n';
  { std::ofstream f(dir, std::ios::binary | std::ios::trunc); f.write(d, std::streamsize(n)); }
  std::ifstream in(dir, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.seekg(0, std::ios::end); auto sz = in.tellg(); in.seekg(0);
  std::vector<char> chunk(size_t(sz > 0 ? sz : 0)); in.read(chunk.data(), std::streamsize(chunk.size()));
  std::fstream f(dir.s, std::ios::in | std::ios::out | std::ios::binary); f.seekp(4); f.write("ab", 2); f.close();
  std::string line; while (std::getline(in, line)) {}
  if (!in.is_open() || !f) {}
}

extern "C" int run_streams() {
  int fail = 0; int t = 0;
  auto ck = [&](bool c) { ++t; if (!c) fail = fail * 100 + t; };
  { std::ostringstream o; o << "a" << 12 << ' ' << -7 << ' ' << 3u << ' ' << 2.5 << ' ' << true << std::string("s") << std::string_view("v");
    ck(o.str() == "a12 -7 3 2.5 1sv"); }
  { std::ostringstream o; o << std::hex << 255 << std::dec << ' ' << 255 << ' ' << std::boolalpha << false; ck(o.str() == "ff 255 false"); }
  { std::ostringstream o; o.precision(3); o << 3.14159 << ' '; o << std::fixed << 2.0; ck(o.str() == "3.14 2.000"); }
  { std::ostringstream o; o << 1.0 / 3.0; ck(o.str() == "0.333333"); }
  { std::stringstream ss("1,22,,333"); std::string tok; std::vector<std::string> v; while (std::getline(ss, tok, ',')) v.push_back(tok);
    ck(v.size() == 4 && v[0] == "1" && v[1] == "22" && v[2].empty() && v[3] == "333"); }
  { std::istringstream is("  42 -3 x 7.5e1 abc"); int a = 0, b = 0, c = 9; double d = 0; std::string w;
    is >> a >> b; ck(a == 42 && b == -3 && is.good());
    is >> c; ck(c == 0 && is.fail()); is.clear(); is >> w; ck(w == "x"); is >> d >> w; ck(d == 75.0 && w == "abc" && is.eof()); }
  { std::istringstream is("1-2"); int a = 0; std::string r; is >> a >> r; ck(a == 1 && r == "-2"); }
  { std::istringstream is("300"); unsigned char uc = 0; short s = 0; is >> s; ck(s == 300); std::istringstream j("70000"); j >> s; ck(j.fail() && s == 32767); (void)uc; }
  { std::stringstream a("hello\nworld"); std::stringstream b; b << a.rdbuf(); ck(b.str() == "hello\nworld"); }
  { std::istringstream is("abc"); std::string s((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>()); ck(s == "abc"); }
  { std::istringstream is("abcdef"); char buf[4] = {}; is.read(buf, 4); ck(is.gcount() == 4 && buf[3] == 'd' && is.tellg() == 4);
    is.read(buf, 4); ck(is.gcount() == 2 && is.eof() && is.fail()); is.clear(); is.seekg(1); ck(is.get() == 'b' && is.peek() == 'c'); }
  { std::ostringstream o("XXXX"); o << "ab"; ck(o.str() == "abXX"); std::ostringstream p("XX", std::ios::ate); p << "y"; ck(p.str() == "XXy"); }
  { std::stringstream s; s << "abc"; s.seekp(1); s << 'Z'; ck(s.str() == "aZc"); std::string x; s >> x; ck(x == "aZc"); s.seekg(0, std::ios::end); ck(s.tellg() == 3); }
  { std::stringstream s; s.write("ab\0c", 4); ck(s.str().size() == 4); std::ostringstream e; e << std::stringstream().rdbuf(); ck(e.fail()); }
  { std::istringstream is("a\nbb\n"); std::string l; int n = 0; while (std::getline(is, l)) ++n; ck(n == 2 && l.empty()); /* standard: the failed third call still erases l */ }
  { std::istringstream is("1 0 2"); bool x = false, y = true, z = false; is >> x >> y; ck(x && !y); is >> z; ck(is.fail()); }
  { std::ostringstream o; o << std::endl << std::flush; ck(o.str() == "\n"); }
  return fail;
}
