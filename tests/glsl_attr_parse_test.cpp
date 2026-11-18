// 2026-11-16 (S5 web): glsl_attr_parse.h (the bounded hand parser that replaced the two
// std::regex patterns in gl33_renderer.cpp canonPullVariant) against those regexes, kept here
// as the reference. Covers every built-in shader file, raw and with includes expanded, plus
// deterministic mutations of chunk.vert around the attribute and main() declarations.
// Native only (the reference needs <regex>); the parser itself is what the web build uses.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "builtin_shaders.h"
#include "litholattice/assets/shader_library.h"
#include "litholattice/render/glsl_attr_parse.h"

namespace litholattice {
namespace {

int g_gpFails = 0;
void gpCheck(bool ok, const std::string& what) {
  if (!ok) { std::printf("  [FAIL] %s\n", what.c_str()); ++g_gpFails; }
}

struct Hit { size_t pos, len; std::string loc, type, name; bool operator==(const Hit&) const = default; };

// ---- reference: the exact regex code that canonPullVariant used before 2026-11-16 ----
const std::regex& refAttrRe() {
  static const std::regex r(R"(layout\s*\(\s*location\s*=\s*([0-3])\s*\)\s*in\s+(uvec[234]|uint)\s+(aPos|aInfo|aShade|aLayerHi)\s*;)");
  return r;
}
const std::regex& refMainRe() {
  static const std::regex r(R"(void\s+main\s*\(\s*(void)?\s*\))");
  return r;
}
std::vector<Hit> refHits(const std::string& vs) {
  std::vector<Hit> h;
  for (auto it = std::sregex_iterator(vs.begin(), vs.end(), refAttrRe()); it != std::sregex_iterator(); ++it)
    h.push_back({size_t(it->position(0)), size_t(it->length(0)), (*it)[1].str(), (*it)[2].str(), (*it)[3].str()});
  return h;
}
bool refMain(const std::string& s, size_t* pos, size_t* len) {
  std::smatch mm;
  if (!std::regex_search(s, mm, refMainRe())) return false;
  *pos = size_t(mm.position(0)); *len = size_t(mm.length(0));
  return true;
}
std::optional<std::string> refRewrite(const std::string& vs) {
  std::string out;
  int found = 0;
  size_t last = 0;
  for (auto it = std::sregex_iterator(vs.begin(), vs.end(), refAttrRe()); it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
    out += vs.substr(last, size_t(m.position(0)) - last);
    out += "layout(location = " + m[1].str() + ") in " + m[2].str() + " " + m[3].str() + "_lkIn; " + m[2].str() + " " + m[3].str() + ";";
    last = size_t(m.position(0) + m.length(0));
    ++found;
  }
  out += vs.substr(last);
  if (found != 4) return std::nullopt;
  std::smatch mm;
  if (!std::regex_search(out, mm, refMainRe())) return std::nullopt;
  out.replace(size_t(mm.position(0)), size_t(mm.length(0)), "void lk_chunk_main()");
  return out;
}
// ---- parser side ----
std::vector<Hit> newHits(const std::string& vs) {
  std::vector<Hit> h;
  glslparse::AttrDecl d;
  size_t last = 0;
  while (last < vs.size() && glslparse::findAttr(vs, last, &d)) {
    h.push_back({d.pos, d.len, std::string(1, d.location), std::string(d.type), std::string(d.name)});
    last = d.pos + d.len;
  }
  return h;
}
bool distinctFour(const std::vector<Hit>& h) {
  if (h.size() != 4) return false;
  for (size_t i = 0; i < 4; ++i)
    for (size_t j = i + 1; j < 4; ++j)
      if (h[i].name == h[j].name) return false;
  return true;
}

int g_cases = 0, g_accepted = 0, g_stricter = 0;
void compareOne(const std::string& label, const std::string& src) {
  ++g_cases;
  const std::vector<Hit> a = refHits(src), b = newHits(src);
  gpCheck(a == b, label + ": attribute matches differ (regex " + std::to_string(a.size()) + ", parser " + std::to_string(b.size()) + ")");
  size_t p1 = 0, l1 = 0, p2 = 0, l2 = 0;
  const bool m1 = refMain(src, &p1, &l1), m2 = glslparse::findMainDecl(src, &p2, &l2);
  gpCheck(m1 == m2 && (!m1 || (p1 == p2 && l1 == l2)), label + ": main() match differs");
  const std::optional<std::string> r = refRewrite(src);
  std::string out, why;
  const bool ok = glslparse::rewriteChunkVert(src, &out, &why);
  if (r && !ok && !distinctFour(a)) { ++g_stricter; return; }  // regex accepted a repeated name; parser rejects by design
  gpCheck(bool(r) == ok, label + ": accept/reject differs");
  if (r && ok) { ++g_accepted; gpCheck(*r == out, label + ": rewritten source differs"); }
  if (!ok) gpCheck(!why.empty(), label + ": reject without a reason");
}

uint64_t g_lcg = 0x9E3779B97F4A7C15ull;
uint32_t nextU32() { g_lcg = g_lcg * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(g_lcg >> 33); }

}  // namespace

int testGlslAttrParse() {
  std::printf("glsl attribute parser vs the old regexes (gl33 canonPullVariant)\n");
  g_gpFails = 0; g_cases = 0; g_accepted = 0; g_stricter = 0;
  auto builtin = [](std::string_view n) -> std::optional<std::string> {
    for (const BuiltinShaderFile& f : kBuiltinShaderFiles) if (n == f.name) return std::string(f.source);
    return std::nullopt;
  };
  int files = 0;
  std::string chunkVert;
  for (const BuiltinShaderFile& f : kBuiltinShaderFiles) {
    ++files;
    compareOne(std::string("raw ") + f.name, f.source);
    std::string err;
    const std::optional<std::string> ex = expandShaderIncludes(f.source, builtin, &err);
    if (ex) compareOne(std::string("expanded ") + f.name, *ex);
    if (std::string_view(f.name) == "chunk.vert" && ex) chunkVert = *ex;
  }
  gpCheck(files > 0, "built-in shader table is empty");
  gpCheck(!chunkVert.empty(), "chunk.vert missing from the built-ins");
  {  // the real input must be accepted, exactly as before
    std::string out, why;
    gpCheck(glslparse::rewriteChunkVert(chunkVert, &out, &why), "chunk.vert rejected: " + why);
  }
  const int builtinCases = g_cases;
  // Mutations of chunk.vert near the declarations: spaces of every kind, digits, brackets,
  // letters of the keywords, deletions and duplications.
  const std::string alphabet = std::string(" \t\n\r\v\f()=;0123456789") + "uvecintlayoutmainvoidaPosInfoShadeLayerHi_x";
  std::vector<size_t> anchors;
  for (const char* key : {"layout", "void main", "in uvec", "in uint", "aLayerHi", "aShade"})
    for (size_t p = chunkVert.find(key); p != std::string::npos; p = chunkVert.find(key, p + 1)) anchors.push_back(p);
  if (!anchors.empty()) {
    for (int k = 0; k < 20000; ++k) {
      std::string m = chunkVert;
      const int edits = 1 + int(nextU32() % 3);
      for (int e = 0; e < edits; ++e) {
        const size_t at = std::min(m.size(), anchors[nextU32() % anchors.size()] + nextU32() % 40);
        const char ch = alphabet[nextU32() % alphabet.size()];
        switch (nextU32() % 4) {
          case 0: m.insert(at, 1, ch); break;
          case 1: if (at < m.size()) m.erase(at, 1); break;
          case 2: if (at < m.size()) m[at] = ch; break;
          default: { const size_t n = std::min<size_t>(m.size() - at, 1 + nextU32() % 60); m.insert(at, m.substr(at, n)); } break;
        }
      }
      compareOne("mutation " + std::to_string(k), m);
    }
  }
  // Exhaustive single-byte edits over every attribute declaration and the main() line:
  // each byte replaced by, and each gap given, every alphabet character, plus each byte deleted.
  {
    std::vector<std::pair<size_t, size_t>> spans;
    for (const Hit& h : refHits(chunkVert)) spans.push_back({h.pos, h.len});
    size_t mp = 0, ml = 0;
    if (refMain(chunkVert, &mp, &ml)) spans.push_back({mp, ml});
    for (const auto& [p0, len] : spans)
      for (size_t at = p0; at <= p0 + len && at <= chunkVert.size(); ++at) {
        for (char ch : alphabet) {
          if (at < chunkVert.size()) { std::string m = chunkVert; m[at] = ch; compareOne("subst " + std::to_string(at), m); }
          std::string m = chunkVert; m.insert(at, 1, ch); compareOne("insert " + std::to_string(at), m);
        }
        if (at < chunkVert.size()) { std::string m = chunkVert; m.erase(at, 1); compareOne("delete " + std::to_string(at), m); }
      }
  }
  // Edge inputs: empty, truncated at every byte of the first declaration, oversize.
  compareOne("empty", "");
  const size_t d0 = chunkVert.find("layout");
  if (d0 != std::string::npos)
    for (size_t n = d0; n < std::min(chunkVert.size(), d0 + 80); ++n) compareOne("truncated " + std::to_string(n), chunkVert.substr(0, n));
  {
    std::string big(glslparse::kMaxGlslSource + 1, ' '), out, why;
    gpCheck(!glslparse::rewriteChunkVert(big, &out, &why) && !why.empty(), "oversize source not rejected");
  }
  std::printf("  glsl_attr_parse: %d built-in shader files (%d raw+expanded cases), %d cases total, %d accepted, %d stricter rejects, %d mismatches\n",
              files, builtinCases, g_cases, g_accepted, g_stricter, g_gpFails);
  std::printf("  [%s] parser == regex over every built-in shader and the mutations\n", g_gpFails == 0 ? "PASS" : "FAIL");
  return g_gpFails;
}

}  // namespace litholattice
int testGlslAttrParseEntry() { return litholattice::testGlslAttrParse(); }
