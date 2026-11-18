// Host harness for lk_png.h (not part of any build; compile on a dev host):
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
//       -I engine/src/assets/include engine/src/assets/fuzz/lk_png_check.cpp -o lk_png_check
// Modes:
//   lk_png_check decode <files...>     one line per file: "<file> <w> <h> <fnv1a64 of RGBA>" or "<file> FAIL <reason>"
//   lk_png_check malformed             built-in malformed cases; every one must be rejected (rc 0 = all rejected)
//   lk_png_check fuzz|fuzzfix <iters> <files…> mutation fuzz (fuzzfix re-seals chunk CRCs after mutating) from a corpus under the sanitizers; any accepted output
//                                       must have size w*h*4 within the caps (rc 0 = no violation)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "litholattice/assets/lk_png.h"

using namespace litholattice;
using Bytes = std::vector<uint8_t>;

static Bytes readFile(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}
static uint64_t fnv(const Bytes& b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
  return h;
}
// Deterministic generator for the fuzzer's mutations only (never feeds a hash).
struct XorShift { uint64_t s; uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } };

// ---- a tiny PNG writer (stored deflate blocks) for crafted cases ------------
static void put32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
static void chunk(Bytes& out, const char* type, const Bytes& body) {
  put32(out, uint32_t(body.size()));
  Bytes tb(type, type + 4);
  tb.insert(tb.end(), body.begin(), body.end());
  out.insert(out.end(), tb.begin(), tb.end());
  put32(out, lkpng::detail::crc32_update(0, tb.data(), tb.size()));
}
static Bytes zlibStored(const Bytes& raw) {
  Bytes z = {0x78, 0x01};
  size_t p = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - p);
    z.push_back(p + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
    p += n;
  } while (p < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  put32(z, (b << 16) | a);
  return z;
}
static Bytes ihdr(uint32_t w, uint32_t h, uint8_t depth, uint8_t ct, uint8_t il = 0) {
  Bytes b; put32(b, w); put32(b, h); b.push_back(depth); b.push_back(ct); b.push_back(0); b.push_back(0); b.push_back(il);
  return b;
}
static Bytes png(const Bytes& hdr, const Bytes& z, bool iend = true) {
  Bytes o = {137, 80, 78, 71, 13, 10, 26, 10};
  chunk(o, "IHDR", hdr);
  chunk(o, "IDAT", z);
  if (iend) chunk(o, "IEND", {});
  return o;
}
static Bytes rawRGBA(uint32_t w, uint32_t h, uint8_t filter = 0) {
  Bytes r;
  for (uint32_t y = 0; y < h; ++y) { r.push_back(filter); for (uint32_t x = 0; x < w * 4; ++x) r.push_back(uint8_t(x + y)); }
  return r;
}

static int malformed() {
  struct Case { const char* name; Bytes data; };
  std::vector<Case> cs;
  const Bytes good = png(ihdr(4, 4, 8, 6), zlibStored(rawRGBA(4, 4)));
  cs.push_back({"empty", {}});
  cs.push_back({"signature only", Bytes(good.begin(), good.begin() + 8)});
  for (size_t cut : {9u, 20u, 33u, 40u, 50u}) if (cut < good.size()) cs.push_back({"truncated", Bytes(good.begin(), good.begin() + cut)});
  cs.push_back({"truncated before IEND", Bytes(good.begin(), good.end() - 12)});
  { Bytes b = good; b[0] = 0; cs.push_back({"bad signature", b}); }
  { Bytes b = good; b[8 + 8 + 13] ^= 1; cs.push_back({"IHDR CRC flipped", b}); }
  cs.push_back({"zero width", png(ihdr(0, 4, 8, 6), zlibStored(rawRGBA(0, 4)))});
  cs.push_back({"too wide", png(ihdr(9000, 1, 8, 6), zlibStored(rawRGBA(4, 1)))});
  cs.push_back({"too many pixels", png(ihdr(8192, 8192, 8, 6), zlibStored(rawRGBA(4, 1)))});
  cs.push_back({"huge dims (u32 max)", png(ihdr(0xFFFFFFFFu, 0xFFFFFFFFu, 8, 6), zlibStored(rawRGBA(4, 1)))});
  cs.push_back({"16-bit", png(ihdr(4, 4, 16, 6), zlibStored(rawRGBA(8, 4)))});
  cs.push_back({"interlaced", png(ihdr(4, 4, 8, 6, 1), zlibStored(rawRGBA(4, 4)))});
  cs.push_back({"colour type 5", png(ihdr(4, 4, 8, 5), zlibStored(rawRGBA(4, 4)))});
  cs.push_back({"palette without PLTE", png(ihdr(4, 4, 8, 3), zlibStored(Bytes(20, 0)))});
  cs.push_back({"data short (bomb guard)", png(ihdr(4, 4, 8, 6), zlibStored(rawRGBA(4, 3)))});
  cs.push_back({"data long (bomb guard)", png(ihdr(4, 4, 8, 6), zlibStored(rawRGBA(4, 400)))});
  cs.push_back({"bad filter byte", png(ihdr(4, 4, 8, 6), zlibStored(rawRGBA(4, 4, 5)))});
  { Bytes z = zlibStored(rawRGBA(4, 4)); z.back() ^= 1; cs.push_back({"Adler-32 mismatch", png(ihdr(4, 4, 8, 6), z)}); }
  { Bytes z = zlibStored(rawRGBA(4, 4)); z[1] = 0x02; cs.push_back({"zlib header check", png(ihdr(4, 4, 8, 6), z)}); }
  { Bytes z = zlibStored(rawRGBA(4, 4)); z[1] = uint8_t(0x20 | (31 - (((0x78 << 8) | 0x20) % 31))); cs.push_back({"preset dictionary", png(ihdr(4, 4, 8, 6), z)}); }
  { Bytes z = zlibStored(rawRGBA(4, 4)); z[3] ^= 0xFF; cs.push_back({"stored LEN/NLEN mismatch", png(ihdr(4, 4, 8, 6), z)}); }
  { Bytes z = {0x78, 0x01, 0x07, 0, 0, 0, 0, 0}; cs.push_back({"block type 3", png(ihdr(4, 4, 8, 6), z)}); }
  // fixed block: literal then a length/distance that reaches before the start
  { Bytes z = {0x78, 0x01, 0x03, 0x02, 0x00, 0, 0, 0, 0}; cs.push_back({"distance before start", png(ihdr(4, 4, 8, 6), z)}); }
  { Bytes o = {137, 80, 78, 71, 13, 10, 26, 10}; chunk(o, "IDAT", Bytes(40, 0)); cs.push_back({"IDAT before IHDR", o}); }
  { Bytes o = {137, 80, 78, 71, 13, 10, 26, 10}; chunk(o, "IHDR", ihdr(4, 4, 8, 6)); chunk(o, "IHDR", ihdr(4, 4, 8, 6)); cs.push_back({"two IHDR", o}); }
  { Bytes o = {137, 80, 78, 71, 13, 10, 26, 10}; chunk(o, "IHDR", ihdr(4, 4, 8, 6)); chunk(o, "IEND", {}); cs.push_back({"no IDAT", o}); }
  { Bytes o = {137, 80, 78, 71, 13, 10, 26, 10}; chunk(o, "IHDR", ihdr(4, 4, 8, 6)); chunk(o, "ZZZZ", {1}); chunk(o, "IDAT", zlibStored(rawRGBA(4, 4))); chunk(o, "IEND", {}); cs.push_back({"unknown critical chunk", o}); }
  { Bytes o = {137, 80, 78, 71, 13, 10, 26, 10}; chunk(o, "IHDR", ihdr(1, 1, 8, 3)); chunk(o, "PLTE", {1, 2, 3}); chunk(o, "IDAT", zlibStored({0, 7})); chunk(o, "IEND", {}); cs.push_back({"palette index out of range", o}); }
  { Bytes o = {137, 80, 78, 71, 13, 10, 26, 10}; chunk(o, "IHDR", ihdr(4, 4, 8, 6)); Bytes z = zlibStored(rawRGBA(4, 4)); chunk(o, "IDAT", Bytes(z.begin(), z.begin() + 10)); chunk(o, "tEXt", {65}); chunk(o, "IDAT", Bytes(z.begin() + 10, z.end())); chunk(o, "IEND", {}); cs.push_back({"IDAT not consecutive", o}); }
  { Bytes b = good; b[8] = 0x7F; b[9] = b[10] = b[11] = 0xFF; cs.push_back({"chunk length past end", b}); }
  int accepted = 0;
  for (const Case& c : cs) {
    lkpng::Image img; const char* err = nullptr;
    const bool ok = lkpng::decode(c.data.data(), c.data.size(), img, &err);
    std::printf("%-28s %s\n", c.name, ok ? "ACCEPTED (BUG)" : err);
    if (ok) ++accepted;
  }
  // The good image must decode, or the cases above prove nothing.
  lkpng::Image img; const char* err = nullptr;
  const bool good_ok = lkpng::decode(good.data(), good.size(), img, &err) && img.w == 4 && img.h == 4 && img.rgba[4] == 4;
  std::printf("control (well-formed)        %s\n", good_ok ? "decoded" : err);
  std::printf("malformed: %zu cases, %d accepted\n", cs.size(), accepted);
  return (accepted || !good_ok) ? 1 : 0;
}

// Recompute every chunk CRC that can be located, so mutations reach the chunk
// parser, inflate and the unfilter instead of all stopping at the CRC check.
static void fixCrcs(Bytes& b) {
  size_t p = 8;
  while (b.size() >= 12 && p <= b.size() - 12) {
    const uint32_t len = lkpng::detail::be32(b.data() + p);
    if (len > b.size() - p - 12) return;
    const uint32_t c = lkpng::detail::crc32_update(0, b.data() + p + 4, size_t(len) + 4);
    for (int k = 0; k < 4; ++k) b[p + 8 + len + k] = uint8_t(c >> (24 - 8 * k));
    p += size_t(len) + 12;
  }
}

static int fuzz(long iters, int nfiles, char** files, bool fix) {
  std::vector<Bytes> corpus;
  for (int i = 0; i < nfiles; ++i) { Bytes b = readFile(files[i]); if (!b.empty() && b.size() < (1u << 20)) corpus.push_back(b); }
  if (corpus.empty()) { std::printf("fuzz: empty corpus\n"); return 2; }
  XorShift r{0x9E3779B97F4A7C15ull};
  long accepted = 0, rejected = 0;
  for (long it = 0; it < iters; ++it) {
    Bytes b = corpus[r.next() % corpus.size()];
    const int nm = 1 + int(r.next() % 8);
    for (int m = 0; m < nm && !b.empty(); ++m) {
      const size_t at = r.next() % b.size();
      switch (r.next() % 6) {
        case 0: b[at] ^= uint8_t(1u << (r.next() % 8)); break;
        case 1: b[at] = uint8_t(r.next()); break;
        case 2: b.resize(at); break;                                   // truncate
        case 3: b.insert(b.begin() + at, uint8_t(r.next())); break;   // insert
        case 4: b.erase(b.begin() + at); break;                        // delete
        default: { const uint32_t v = uint32_t(r.next()); for (int k = 0; k < 4 && at + k < b.size(); ++k) b[at + k] = uint8_t(v >> (8 * k)); }
      }
    }
    if (fix) fixCrcs(b);
    lkpng::Image img; const char* err = nullptr;
    if (lkpng::decode(b.data(), b.size(), img, &err)) {
      ++accepted;
      if (img.w == 0 || img.h == 0 || img.w > lkpng::kMaxSide || img.h > lkpng::kMaxSide ||
          img.rgba.size() != size_t(img.w) * img.h * 4) { std::printf("fuzz: VIOLATION at iteration %ld\n", it); return 1; }
    } else {
      ++rejected;
      if (!err || !*err) { std::printf("fuzz: rejection without a reason at %ld\n", it); return 1; }
    }
  }
  std::printf("fuzz%s: %ld iterations, %ld accepted, %ld rejected, 0 violations\n", fix ? " (CRCs fixed up)" : "", iters, accepted, rejected);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "malformed")) return malformed();
  if (argc >= 4 && !std::strcmp(argv[1], "fuzz")) return fuzz(std::atol(argv[2]), argc - 3, argv + 3, false);
  if (argc >= 4 && !std::strcmp(argv[1], "fuzzfix")) return fuzz(std::atol(argv[2]), argc - 3, argv + 3, true);
  if (argc >= 2 && !std::strcmp(argv[1], "decode")) {
    for (int i = 2; i < argc; ++i) {
      const Bytes b = readFile(argv[i]);
      lkpng::Image img; const char* err = nullptr;
      if (lkpng::decode(b.data(), b.size(), img, &err)) std::printf("%s %u %u %016llx\n", argv[i], img.w, img.h, (unsigned long long)fnv(img.rgba));
      else std::printf("%s FAIL %s\n", argv[i], err);
    }
    return 0;
  }
  std::fprintf(stderr, "usage: lk_png_check decode <files...> | malformed | fuzz <iters> <files...>\n");
  return 2;
}
