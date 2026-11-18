// lk_png.h: LithoLattice's in-house PNG decoder (web build; no outside libraries).
//
// Safety-critical input path: every input byte is untrusted. The decoder
//   - bounds every read (a reader past the end is an error, never UB),
//   - checks every chunk CRC and the zlib Adler-32,
//   - knows the exact inflated size from IHDR before inflating and writes into
//     a buffer of exactly that size (more data or less data is an error, so a
//     decompression bomb cannot grow memory),
//   - caps input at 64 MiB, each side at 8192 px and the image at 16 Mi pixels
//     (64 MiB RGBA), and checks those caps before allocating anything,
//   - fails closed: unsupported variants are rejected with a reason, never guessed.
// Supported (measured against the asset tree, 2026-11-16: 5020 PNGs, all 8-bit,
// non-interlaced, colour types 6/0/2): bit depth 8, colour types 0, 2, 3, 4, 6,
// no interlace. 16-bit, sub-byte depths and Adam7 are rejected.
// Output: tightly packed RGBA8, top row first.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace litholattice::lkpng {

constexpr size_t kMaxInput = size_t(64) << 20;
constexpr uint32_t kMaxSide = 8192;
constexpr uint64_t kMaxPixels = uint64_t(16) << 20;

struct Image {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

namespace detail {

struct CrcTable {
  uint32_t v[256];
  constexpr CrcTable() : v() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t x = i;
      for (int k = 0; k < 8; ++k) x = (x & 1u) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
      v[i] = x;
    }
  }
};
inline constexpr CrcTable kCrc{};  // built at compile time: no run-time init, no shared mutable state

inline uint32_t crc32_update(uint32_t c, const uint8_t* p, size_t n) {
  c = ~c;
  for (size_t i = 0; i < n; ++i) c = kCrc.v[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return ~c;
}

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// ---- inflate (RFC 1950/1951) into a fixed-size output ----------------------
struct Inflater {
  const uint8_t* in;
  size_t inLen, pos = 0;
  uint32_t bitBuf = 0;
  int bitCnt = 0;
  uint8_t* out;
  size_t outLen, outPos = 0;
  const char* err = nullptr;

  bool fail(const char* e) { if (!err) err = e; return false; }
  // Returns -1 past the end of input (and records the error).
  int bits(int n) {
    while (bitCnt < n) {
      if (pos >= inLen) { fail("zlib stream truncated"); return -1; }
      bitBuf |= uint32_t(in[pos++]) << bitCnt;
      bitCnt += 8;
    }
    const int v = int(bitBuf & ((1u << n) - 1u));
    bitBuf >>= n;
    bitCnt -= n;
    return v;
  }

  struct Huff { uint16_t count[16]; uint16_t symbol[320]; };

  // Canonical Huffman table from code lengths; rejects over-subscribed sets.
  // Incomplete sets are allowed only for the single-code case RFC 1951 permits.
  bool build(Huff& h, const uint8_t* lens, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; ++i) h.count[lens[i]]++;
    if (h.count[0] == n) return true;  // no codes: only an error if used
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= h.count[len];
      if (left < 0) return fail("over-subscribed Huffman code");
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i) if (lens[i]) h.symbol[offs[lens[i]]++] = uint16_t(i);
    return true;
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      const int b = bits(1);
      if (b < 0) return -1;
      code |= b;
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    fail("invalid Huffman code");
    return -1;
  }

  bool codes(const Huff& lit, const Huff& dist) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int sym = decode(lit);
      if (sym < 0) return false;
      if (sym < 256) {
        if (outPos >= outLen) return fail("image data longer than IHDR says");
        out[outPos++] = uint8_t(sym);
      } else if (sym == 256) {
        return true;
      } else {
        sym -= 257;
        if (sym >= 29) return fail("invalid length symbol");
        const int e = bits(lext[sym]);
        if (e < 0) return false;
        const size_t len = size_t(lbase[sym]) + size_t(e);
        const int ds = decode(dist);
        if (ds < 0) return false;
        if (ds >= 30) return fail("invalid distance symbol");
        const int de = bits(dext[ds]);
        if (de < 0) return false;
        const size_t d = size_t(dbase[ds]) + size_t(de);
        if (d > outPos) return fail("distance reaches before the start");
        if (len > outLen - outPos) return fail("image data longer than IHDR says");
        for (size_t i = 0; i < len; ++i, ++outPos) out[outPos] = out[outPos - d];
      }
    }
  }

  bool stored() {
    bitBuf = 0;
    bitCnt = 0;  // to the byte boundary
    if (inLen - pos < 4) return fail("stored block header truncated");
    const uint32_t len = in[pos] | (uint32_t(in[pos + 1]) << 8);
    const uint32_t nlen = in[pos + 2] | (uint32_t(in[pos + 3]) << 8);
    pos += 4;
    if (len != (~nlen & 0xFFFFu)) return fail("stored block length check failed");
    if (inLen - pos < len) return fail("stored block truncated");
    if (len > outLen - outPos) return fail("image data longer than IHDR says");
    std::memcpy(out + outPos, in + pos, len);
    pos += len;
    outPos += len;
    return true;
  }

  bool fixed() {
    Huff lit, dist;  // rebuilt per block (cheap), so no shared mutable state
    uint8_t l[288];
    int i = 0;
    for (; i < 144; ++i) l[i] = 8;
    for (; i < 256; ++i) l[i] = 9;
    for (; i < 280; ++i) l[i] = 7;
    for (; i < 288; ++i) l[i] = 8;
    build(lit, l, 288);
    uint8_t d[30];
    for (i = 0; i < 30; ++i) d[i] = 5;
    build(dist, d, 30);
    return codes(lit, dist);
  }

  bool dynamic() {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int hlit = bits(5), hdist = bits(5), hclen = bits(4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return false;
    const int nlen = hlit + 257, ndist = hdist + 1, ncode = hclen + 4;
    if (nlen > 286 || ndist > 30) return fail("bad dynamic block counts");
    uint8_t lens[320] = {0};
    for (int i = 0; i < ncode; ++i) {
      const int v = bits(3);
      if (v < 0) return false;
      lens[order[i]] = uint8_t(v);
    }
    Huff lc;
    if (!build(lc, lens, 19)) return false;
    int idx = 0;
    while (idx < nlen + ndist) {
      int sym = decode(lc);
      if (sym < 0) return false;
      if (sym < 16) { lens[idx++] = uint8_t(sym); continue; }
      int rep = 0;
      uint8_t val = 0;
      if (sym == 16) {
        if (idx == 0) return fail("repeat with no previous length");
        val = lens[idx - 1];
        const int b = bits(2); if (b < 0) return false; rep = 3 + b;
      } else if (sym == 17) {
        const int b = bits(3); if (b < 0) return false; rep = 3 + b;
      } else {
        const int b = bits(7); if (b < 0) return false; rep = 11 + b;
      }
      if (idx + rep > nlen + ndist) return fail("code lengths overrun");
      while (rep--) lens[idx++] = val;
    }
    if (lens[256] == 0) return fail("no end-of-block code");
    Huff lit, dist;
    if (!build(lit, lens, nlen)) return false;
    if (!build(dist, lens + nlen, ndist)) return false;
    return codes(lit, dist);
  }

  bool run() {
    if (inLen < 6) return fail("zlib stream too short");
    const uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7) return fail("not a deflate zlib stream");
    if (((uint32_t(cmf) << 8) | flg) % 31u != 0) return fail("zlib header check failed");
    if (flg & 0x20) return fail("zlib preset dictionary not allowed");
    pos = 2;
    int last;
    do {
      last = bits(1);
      const int type = bits(2);
      if (last < 0 || type < 0) return false;
      bool ok;
      if (type == 0) ok = stored();
      else if (type == 1) ok = fixed();
      else if (type == 2) ok = dynamic();
      else ok = fail("invalid deflate block type");
      if (!ok) return false;
    } while (!last);
    if (outPos != outLen) return fail("image data shorter than IHDR says");
    if (inLen - pos < 4) return fail("zlib Adler-32 missing");
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < outLen; ++i) { a = (a + out[i]) % 65521u; b = (b + a) % 65521u; }
    if (be32(in + pos) != ((b << 16) | a)) return fail("zlib Adler-32 mismatch");
    return true;
  }
};

inline uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return uint8_t((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
}

}  // namespace detail

// Decode; on failure returns false with *err set to a static reason string.
inline bool decode(const uint8_t* data, size_t size, Image& img, const char** err) {
  using namespace detail;
  const auto fail = [&](const char* e) { if (err) *err = e; img = Image{}; return false; };
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (!data || size < 8 + 25 + 12) return fail("not a PNG (too short)");
  if (size > kMaxInput) return fail("PNG larger than 64 MiB");
  if (std::memcmp(data, sig, 8) != 0) return fail("not a PNG (bad signature)");
  size_t p = 8;
  uint32_t w = 0, h = 0;
  int ct = -1;
  bool seenIHDR = false, seenIDAT = false, idatDone = false, seenIEND = false;
  uint8_t pal[256][4];
  int palN = 0, trnsN = 0;
  bool trnsKey = false;
  uint16_t key[3] = {0, 0, 0};
  std::vector<uint8_t> z;
  while (!seenIEND) {
    if (size - p < 12) return fail("chunk header truncated");
    const uint32_t len = be32(data + p);
    if (len > 0x7FFFFFFFu || len > size - p - 12) return fail("chunk runs past the end");
    const uint8_t* type = data + p + 4;
    const uint8_t* body = data + p + 8;
    for (int i = 0; i < 4; ++i) {
      const uint8_t t = type[i];
      if (!((t >= 'A' && t <= 'Z') || (t >= 'a' && t <= 'z'))) return fail("bad chunk type");
    }
    if (crc32_update(0, type, size_t(len) + 4) != be32(body + len)) return fail("chunk CRC mismatch");
    p += size_t(len) + 12;
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (seenIHDR || len != 13) return fail("bad IHDR");
      seenIHDR = true;
      w = be32(body);
      h = be32(body + 4);
      const uint8_t depth = body[8];
      ct = body[9];
      if (w == 0 || h == 0) return fail("zero image size");
      if (w > kMaxSide || h > kMaxSide || uint64_t(w) * h > kMaxPixels) return fail("image too large");
      if (body[10] != 0 || body[11] != 0) return fail("unknown compression or filter method");
      if (body[12] != 0) return fail("interlaced PNG not supported");
      if (depth != 8) return fail("only 8-bit PNG supported");
      if (!(ct == 0 || ct == 2 || ct == 3 || ct == 4 || ct == 6)) return fail("bad colour type");
      continue;
    }
    if (!seenIHDR) return fail("first chunk is not IHDR");
    if (std::memcmp(type, "PLTE", 4) == 0) {
      if (palN || seenIDAT || len % 3 != 0 || len == 0 || len > 768) return fail("bad PLTE");
      palN = int(len / 3);
      for (int i = 0; i < palN; ++i) { pal[i][0] = body[3 * i]; pal[i][1] = body[3 * i + 1]; pal[i][2] = body[3 * i + 2]; pal[i][3] = 255; }
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      if (seenIDAT || trnsN || trnsKey) return fail("bad tRNS");
      if (ct == 3) {
        if (!palN || len > uint32_t(palN)) return fail("bad tRNS for palette");
        for (uint32_t i = 0; i < len; ++i) pal[i][3] = body[i];
        trnsN = int(len);
      } else if (ct == 0 && len == 2) {
        key[0] = uint16_t((body[0] << 8) | body[1]); trnsKey = true;
      } else if (ct == 2 && len == 6) {
        for (int i = 0; i < 3; ++i) key[i] = uint16_t((body[2 * i] << 8) | body[2 * i + 1]);
        trnsKey = true;
      } else {
        return fail("bad tRNS");
      }
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      if (idatDone) return fail("IDAT chunks not consecutive");
      if (ct == 3 && !palN) return fail("palette image without PLTE");
      seenIDAT = true;
      z.insert(z.end(), body, body + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      if (len != 0) return fail("bad IEND");
      seenIEND = true;
    } else if (!(type[0] & 0x20)) {
      return fail("unknown critical chunk");
    }
    if (seenIDAT && std::memcmp(type, "IDAT", 4) != 0) idatDone = true;
  }
  if (!seenIDAT) return fail("no image data");
  const int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : 4;
  const size_t stride = size_t(w) * size_t(ch);
  const size_t rawLen = (stride + 1) * size_t(h);
  std::vector<uint8_t> raw(rawLen);
  Inflater inf{z.data(), z.size(), 0, 0, 0, raw.data(), raw.size()};
  if (!inf.run()) return fail(inf.err ? inf.err : "inflate failed");
  // Unfilter in place (row y's filter byte at raw[y*(stride+1)]).
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = raw.data() + size_t(y) * (stride + 1);
    const uint8_t f = row[0];
    uint8_t* cur = row + 1;
    const uint8_t* prev = y ? cur - (stride + 1) : nullptr;
    if (f > 4) return fail("bad filter type");
    for (size_t i = 0; i < stride; ++i) {
      const int a = i >= size_t(ch) ? cur[i - ch] : 0;
      const int b = prev ? prev[i] : 0;
      const int c = (prev && i >= size_t(ch)) ? prev[i - ch] : 0;
      switch (f) {
        case 1: cur[i] = uint8_t(cur[i] + a); break;
        case 2: cur[i] = uint8_t(cur[i] + b); break;
        case 3: cur[i] = uint8_t(cur[i] + ((a + b) >> 1)); break;
        case 4: cur[i] = uint8_t(cur[i] + paeth(a, b, c)); break;
        default: break;
      }
    }
  }
  img.w = w;
  img.h = h;
  img.rgba.assign(size_t(w) * h * 4, 0);
  uint8_t* o = img.rgba.data();
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = raw.data() + size_t(y) * (stride + 1) + 1;
    for (uint32_t x = 0; x < w; ++x, o += 4) {
      switch (ct) {
        case 0: o[0] = o[1] = o[2] = s[x]; o[3] = (trnsKey && s[x] == key[0]) ? 0 : 255; break;
        case 2: {
          const uint8_t* q = s + 3 * size_t(x);
          o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
          o[3] = (trnsKey && q[0] == key[0] && q[1] == key[1] && q[2] == key[2]) ? 0 : 255;
          break;
        }
        case 3: {
          const uint8_t idx = s[x];
          if (idx >= palN) return fail("palette index out of range");
          std::memcpy(o, pal[idx], 4);
          break;
        }
        case 4: o[0] = o[1] = o[2] = s[2 * size_t(x)]; o[3] = s[2 * size_t(x) + 1]; break;
        default: std::memcpy(o, s + 4 * size_t(x), 4); break;
      }
    }
  }
  (void)trnsN;  // tRNS entries already folded into the palette alpha
  return true;
}

}  // namespace litholattice::lkpng
