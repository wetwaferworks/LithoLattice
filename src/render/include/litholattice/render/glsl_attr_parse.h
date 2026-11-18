// 2026-11-16 (S5 web): bounded hand parser for the GL TBO uploader's chunk.vert rewrite
// (gl33_renderer.cpp canonPullVariant, LL_GL_CANON=1). Replaces two std::regex patterns
// (no <regex> in LithoRoot) and accepts exactly what they accepted:
//   attribute: layout\s*\(\s*location\s*=\s*([0-3])\s*\)\s*in\s+(uvec[234]|uint)\s+(aPos|aInfo|aShade|aLayerHi)\s*;
//   main:      void\s+main\s*\(\s*(void)?\s*\)
// \s is the C locale's space set (space, \t, \n, \v, \f, \r), as std::regex uses for char.
// Matches are leftmost and non-overlapping, scanned like std::sregex_iterator / regex_search.
// Bounded and validated: every read is checked against the view's size, input above
// kMaxGlslSource is rejected, and the rewrite additionally rejects (stricter than the regex
// path) a source whose four attributes are not each declared exactly once.
// engine/tests/glsl_attr_parse_test.cpp compares this with the old regexes over every
// built-in shader. Header-only, no allocation except the output string.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace litholattice::glslparse {

inline constexpr std::size_t kMaxGlslSource = std::size_t(1) << 20;  // 1 MiB; chunk.vert is ~3 KB

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

/// Read cursor over a string_view; every read is bounds-checked.
struct Cursor {
  std::string_view s;
  std::size_t i = 0;
  bool lit(std::string_view t) {
    if (i > s.size() || s.size() - i < t.size()) return false;
    if (s.substr(i, t.size()) != t) return false;
    i += t.size();
    return true;
  }
  std::size_t spaces() {  // \s*: returns how many were skipped
    const std::size_t from = i;
    while (i < s.size() && isSpace(s[i])) ++i;
    return i - from;
  }
  bool oneOf(char lo, char hi, char* got) {
    if (i >= s.size() || s[i] < lo || s[i] > hi) return false;
    *got = s[i++];
    return true;
  }
};

/// One attribute declaration: [pos, pos+len) in the source, location digit, type and name
/// (views into the source).
struct AttrDecl {
  std::size_t pos = 0, len = 0;
  char location = 0;
  std::string_view type, name;
};

/// The attribute pattern anchored at p (no search). Greedy \s* is exact here: no token after
/// a run of spaces starts with a space, so the regex never needs to backtrack into one.
inline bool matchAttrAt(std::string_view s, std::size_t p, AttrDecl* d) {
  Cursor c{s, p};
  if (!c.lit("layout")) return false;
  c.spaces();
  if (!c.lit("(")) return false;
  c.spaces();
  if (!c.lit("location")) return false;
  c.spaces();
  if (!c.lit("=")) return false;
  c.spaces();
  char loc = 0;
  if (!c.oneOf('0', '3', &loc)) return false;
  c.spaces();
  if (!c.lit(")")) return false;
  c.spaces();
  if (!c.lit("in")) return false;
  if (c.spaces() == 0) return false;
  const std::size_t t0 = c.i;
  char dim = 0;
  if (c.lit("uvec")) {
    if (!c.oneOf('2', '4', &dim)) return false;
  } else if (!c.lit("uint")) {
    return false;
  }
  const std::string_view type = s.substr(t0, c.i - t0);
  if (c.spaces() == 0) return false;
  const std::size_t n0 = c.i;
  if (!(c.lit("aPos") || c.lit("aInfo") || c.lit("aShade") || c.lit("aLayerHi"))) return false;
  const std::string_view name = s.substr(n0, c.i - n0);
  c.spaces();
  if (!c.lit(";")) return false;
  d->pos = p;
  d->len = c.i - p;
  d->location = loc;
  d->type = type;
  d->name = name;
  return true;
}

/// Leftmost attribute match starting at or after `from` (sregex_iterator order).
inline bool findAttr(std::string_view s, std::size_t from, AttrDecl* d) {
  for (std::size_t p = from; p < s.size(); ++p) {
    if (s[p] != 'l') continue;
    if (matchAttrAt(s, p, d)) return true;
  }
  return false;
}

/// Leftmost `void\s+main\s*\(\s*(void)?\s*\)` in s: position and length.
inline bool findMainDecl(std::string_view s, std::size_t* pos, std::size_t* len) {
  for (std::size_t p = 0; p < s.size(); ++p) {
    if (s[p] != 'v') continue;
    Cursor c{s, p};
    if (!c.lit("void")) continue;
    if (c.spaces() == 0) continue;
    if (!c.lit("main")) continue;
    c.spaces();
    if (!c.lit("(")) continue;
    c.spaces();
    if (c.lit("void")) c.spaces();  // (void)? then \s*; without it the next char must be ')'
    if (!c.lit(")")) continue;
    *pos = p;
    *len = c.i - p;
    return true;
  }
  return false;
}

/// The chunk.vert rewrite: each attribute becomes "layout(location = N) in T aX_lkIn; T aX;"
/// and main() becomes lk_chunk_main(). Returns false with *why on any reject.
inline bool rewriteChunkVert(std::string_view vs, std::string* out, std::string* why) {
  out->clear();
  if (vs.size() > kMaxGlslSource) { *why = "chunk.vert is larger than the 1 MiB parser bound"; return false; }
  int found = 0;
  unsigned seen = 0;  // bit per attribute name
  bool duplicate = false;
  std::size_t last = 0;
  AttrDecl d;
  while (last < vs.size() && findAttr(vs, last, &d)) {
    out->append(vs.substr(last, d.pos - last));
    out->append("layout(location = ").append(1, d.location).append(") in ");
    out->append(d.type).append(" ").append(d.name).append("_lkIn; ");
    out->append(d.type).append(" ").append(d.name).append(";");
    const unsigned bit = d.name == "aPos" ? 1u : d.name == "aInfo" ? 2u : d.name == "aShade" ? 4u : 8u;
    if (seen & bit) duplicate = true;
    seen |= bit;
    last = d.pos + d.len;
    ++found;
  }
  if (last < vs.size()) out->append(vs.substr(last));
  if (found != 4 || duplicate || seen != 15u) {
    *why = "chunk.vert does not declare aPos/aInfo/aShade/aLayerHi as plain attributes";
    return false;
  }
  std::size_t mp = 0, ml = 0;
  if (!findMainDecl(*out, &mp, &ml)) { *why = "no main() in chunk.vert"; return false; }
  out->replace(mp, ml, "void lk_chunk_main()");
  return true;
}

}  // namespace litholattice::glslparse
