// LithoRoot from_chars(float/double, chars_format::general) against libstdc++: every case's
// consumed length, error category (0 ok, 1 invalid_argument, 2 result_out_of_range) and the
// exact bits. The expectations were generated once with g++ 14 / libstdc++ (host). On an
// error the value must stay untouched (-7). Returns the number of mismatches (0 = pass).
#include <charconv>
#include <cstring>
static const char* kFcCases[] = {"0","-0","1","-1","3.25","1e10","1E-5","2.5e+3","1e","1e+","5.","0.5",".5",".","-","+1"," 1","inf","-INF","infinity","Infinit","nan","NaN(abc_1)","nan(","1e400","-1e400","1e-400","4.9e-324","2.2250738585072014e-308","0.1","123456789012345678901234567890","1.7976931348623157e308","1.7976931348623159e308","0x1p3","1.5x","9007199254740993","abc","00012.500e2","-.e1","1.0e-0"};
struct FcExp { int off; int ec; unsigned long long bits; unsigned fbits; int foff; int fec; };
static const FcExp kFcExp[] = {
  {1, 0, 0x0ull, 0x0u, 1, 0},  // "0"
  {2, 0, 0x8000000000000000ull, 0x80000000u, 2, 0},  // "-0"
  {1, 0, 0x3ff0000000000000ull, 0x3f800000u, 1, 0},  // "1"
  {2, 0, 0xbff0000000000000ull, 0xbf800000u, 2, 0},  // "-1"
  {4, 0, 0x400a000000000000ull, 0x40500000u, 4, 0},  // "3.25"
  {4, 0, 0x4202a05f20000000ull, 0x501502f9u, 4, 0},  // "1e10"
  {4, 0, 0x3ee4f8b588e368f1ull, 0x3727c5acu, 4, 0},  // "1E-5"
  {6, 0, 0x40a3880000000000ull, 0x451c4000u, 6, 0},  // "2.5e+3"
  {1, 0, 0x3ff0000000000000ull, 0x3f800000u, 1, 0},  // "1e"
  {1, 0, 0x3ff0000000000000ull, 0x3f800000u, 1, 0},  // "1e+"
  {2, 0, 0x4014000000000000ull, 0x40a00000u, 2, 0},  // "5."
  {3, 0, 0x3fe0000000000000ull, 0x3f000000u, 3, 0},  // "0.5"
  {2, 0, 0x3fe0000000000000ull, 0x3f000000u, 2, 0},  // ".5"
  {0, 1, 0xc01c000000000000ull, 0xc0e00000u, 0, 1},  // "."
  {0, 1, 0xc01c000000000000ull, 0xc0e00000u, 0, 1},  // "-"
  {0, 1, 0xc01c000000000000ull, 0xc0e00000u, 0, 1},  // "+1"
  {0, 1, 0xc01c000000000000ull, 0xc0e00000u, 0, 1},  // " 1"
  {3, 0, 0x7ff0000000000000ull, 0x7f800000u, 3, 0},  // "inf"
  {4, 0, 0xfff0000000000000ull, 0xff800000u, 4, 0},  // "-INF"
  {8, 0, 0x7ff0000000000000ull, 0x7f800000u, 8, 0},  // "infinity"
  {3, 0, 0x7ff0000000000000ull, 0x7f800000u, 3, 0},  // "Infinit"
  {3, 0, 0x7ff8000000000000ull, 0x7fc00000u, 3, 0},  // "nan"
  {10, 0, 0x7ff8000000000000ull, 0x7fc00000u, 10, 0},  // "NaN(abc_1)"
  {3, 0, 0x7ff8000000000000ull, 0x7fc00000u, 3, 0},  // "nan("
  {5, 2, 0xc01c000000000000ull, 0xc0e00000u, 5, 2},  // "1e400"
  {6, 2, 0xc01c000000000000ull, 0xc0e00000u, 6, 2},  // "-1e400"
  {6, 2, 0xc01c000000000000ull, 0xc0e00000u, 6, 2},  // "1e-400"
  {8, 0, 0x1ull, 0xc0e00000u, 8, 2},  // "4.9e-324"
  {23, 0, 0x10000000000000ull, 0xc0e00000u, 23, 2},  // "2.2250738585072014e-308"
  {3, 0, 0x3fb999999999999aull, 0x3dcccccdu, 3, 0},  // "0.1"
  {30, 0, 0x45f8ee90ff6c373eull, 0x6fc77488u, 30, 0},  // "123456789012345678901234567890"
  {22, 0, 0x7fefffffffffffffull, 0xc0e00000u, 22, 2},  // "1.7976931348623157e308"
  {22, 2, 0xc01c000000000000ull, 0xc0e00000u, 22, 2},  // "1.7976931348623159e308"
  {1, 0, 0x0ull, 0x0u, 1, 0},  // "0x1p3"
  {3, 0, 0x3ff8000000000000ull, 0x3fc00000u, 3, 0},  // "1.5x"
  {16, 0, 0x4340000000000000ull, 0x5a000000u, 16, 0},  // "9007199254740993"
  {0, 1, 0xc01c000000000000ull, 0xc0e00000u, 0, 1},  // "abc"
  {11, 0, 0x4093880000000000ull, 0x449c4000u, 11, 0},  // "00012.500e2"
  {0, 1, 0xc01c000000000000ull, 0xc0e00000u, 0, 1},  // "-.e1"
  {6, 0, 0x3ff0000000000000ull, 0x3f800000u, 6, 0},  // "1.0e-0"
};
extern "C" int lk_from_chars_float_check() {
  int bad = 0;
  static_assert(sizeof(kFcCases) / sizeof(kFcCases[0]) == sizeof(kFcExp) / sizeof(kFcExp[0]));
  for (unsigned i = 0; i < sizeof(kFcCases) / sizeof(kFcCases[0]); ++i) {
    const char* s = kFcCases[i];
    const FcExp& e = kFcExp[i];
    auto cat = [](std::errc ec) { return ec == std::errc{} ? 0 : ec == std::errc::invalid_argument ? 1 : 2; };
    double v = -7.0;
    const auto r = std::from_chars(s, s + std::strlen(s), v);
    unsigned long long b; std::memcpy(&b, &v, 8);
    if (int(r.ptr - s) != e.off || cat(r.ec) != e.ec || b != e.bits) ++bad;
    float f = -7.0f;
    const auto rf = std::from_chars(s, s + std::strlen(s), f);
    unsigned fb; std::memcpy(&fb, &f, 4);
    if (int(rf.ptr - s) != e.foff || cat(rf.ec) != e.fec || fb != e.fbits) ++bad;
  }
  return bad;
}
