// lk_svg_parse.h: stage 1 of the in-house SVG decoder (web). It parses only the measured
// subset into a bounded list of shapes, and rejects anything outside it.
//   elements:   svg, path, polygon, line, circle, rect (plus the closing tags)
//   path data:  absolute M L H V Q Z only (lowercase commands are rejected)
//   paint:      fill/stroke as #rgb, #rrggbb or none; fill-opacity, stroke-opacity,
//               opacity, stroke-width, stroke-linejoin, stroke-linecap
//   transform:  translate(x[,y]) optionally followed by scale(sx[,sy]) (the branding SVGs use both)
// Caps: 1 MiB of input, 4096 shapes, 65536 points in total. Any violation returns false
// with an error string and leaves no partial result. No regex, no allocation beyond the
// output vectors, no mutable statics. Rasterising is stage 2 (not written yet).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace litholattice::lksvg {

constexpr size_t kMaxInput = 1u << 20, kMaxShapes = 4096, kMaxPoints = 65536;

struct Paint { bool on = false; uint8_t r = 0, g = 0, b = 0; float a = 1.f; };
struct Pt { float x, y; };
struct Shape {
  Paint fill, stroke; float strokeWidth = 1.f; float opacity = 1.f;
  uint8_t join = 0, cap = 0;      // join: 0 miter 1 round 2 bevel; cap: 0 butt 1 round 2 square
  std::vector<uint32_t> contourStart;  // index into pts where each contour starts
  std::vector<uint8_t> contourClosed;
  std::vector<Pt> pts;            // Q is flattened into lines (fixed 8 steps)
};
struct Doc { float vbX = 0, vbY = 0, vbW = 0, vbH = 0; float width = 0, height = 0; std::vector<Shape> shapes; };

namespace detail {
inline bool isSp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }
// Bounded number parser: [+-]digits[.digits][e[+-]digits]. No locale, no strtod.
inline bool num(std::string_view s, size_t& i, float& out) {
  while (i < s.size() && isSp(s[i])) ++i;
  size_t j = i; bool neg = false;
  if (j < s.size() && (s[j] == '+' || s[j] == '-')) neg = s[j++] == '-';
  double v = 0; int digits = 0;
  while (j < s.size() && s[j] >= '0' && s[j] <= '9') { if (++digits > 12) return false; v = v * 10 + (s[j++] - '0'); }
  if (j < s.size() && s[j] == '.') { ++j; double f = 0.1; while (j < s.size() && s[j] >= '0' && s[j] <= '9') { if (++digits > 24) return false; v += (s[j++] - '0') * f; f *= 0.1; } }
  if (digits == 0) return false;
  if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
    ++j; bool en = false; if (j < s.size() && (s[j] == '+' || s[j] == '-')) en = s[j++] == '-';
    int e = 0, ed = 0; while (j < s.size() && s[j] >= '0' && s[j] <= '9') { if (++ed > 2) return false; e = e * 10 + (s[j++] - '0'); }
    if (ed == 0) return false;
    for (int k = 0; k < e; ++k) v = en ? v / 10 : v * 10;
  }
  if (v > 1e6) return false;
  out = float(neg ? -v : v); i = j; return true;
}
inline int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
inline bool paint(std::string_view v, Paint& p) {
  if (v == "none") { p.on = false; return true; }
  if (v.size() == 7 && v[0] == '#') { int h[6]; for (int k = 0; k < 6; ++k) if ((h[k] = hex(v[1 + k])) < 0) return false;
    p.on = true; p.r = uint8_t(h[0] * 16 + h[1]); p.g = uint8_t(h[2] * 16 + h[3]); p.b = uint8_t(h[4] * 16 + h[5]); return true; }
  if (v.size() == 4 && v[0] == '#') { int h[3]; for (int k = 0; k < 3; ++k) if ((h[k] = hex(v[1 + k])) < 0) return false;
    p.on = true; p.r = uint8_t(h[0] * 17); p.g = uint8_t(h[1] * 17); p.b = uint8_t(h[2] * 17); return true; }
  return false;
}
inline bool unit(std::string_view v, float& f) { size_t i = 0; if (!num(v, i, f)) return false; if (v.substr(i) == "px") i = v.size(); while (i < v.size() && isSp(v[i])) ++i; return i == v.size(); }
inline bool frac(std::string_view v, float& f) { return unit(v, f) && f >= 0.f && f <= 1.f; }
}  // namespace detail

struct Parser {
  std::string_view s; size_t i = 0; size_t points = 0; const char* err = nullptr;
  bool fail(const char* e) { if (!err) err = e; return false; }
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
  bool name(std::string_view& out) { size_t b = i; while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == ':')) ++i; out = s.substr(b, i - b); return i > b && i - b <= 32; }
  bool addPt(Shape& sh, float x, float y) { if (++points > kMaxPoints) return fail("too many points"); sh.pts.push_back({x, y}); return true; }
  bool startContour(Shape& sh) { if (sh.contourStart.size() >= kMaxPoints) return fail("too many contours"); sh.contourStart.push_back(uint32_t(sh.pts.size())); sh.contourClosed.push_back(0); return true; }
  bool path(std::string_view d, Shape& sh) {
    size_t k = 0; float cx = 0, cy = 0, sx = 0, sy = 0; char cmd = 0; bool open = false;
    while (true) {
      while (k < d.size() && detail::isSp(d[k])) ++k;
      if (k >= d.size()) break;
      char c = d[k];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        if (c != 'M' && c != 'L' && c != 'H' && c != 'V' && c != 'Q' && c != 'Z') return fail("path command outside the subset");
        cmd = c; ++k;
        if (c == 'Z') { if (!open) return fail("Z without a contour"); sh.contourClosed.back() = 1; cx = sx; cy = sy; open = false; cmd = 0; continue; }
      } else if (!cmd) return fail("path data without a command");
      float a, b, x1, y1;
      switch (cmd) {
        case 'M': if (!detail::num(d, k, a) || !detail::num(d, k, b)) return fail("bad M");
          if (!startContour(sh) || !addPt(sh, a, b)) return false; cx = sx = a; cy = sy = b; open = true; cmd = 'L'; break;
        case 'L': if (!open) return fail("L before M"); if (!detail::num(d, k, a) || !detail::num(d, k, b)) return fail("bad L");
          if (!addPt(sh, a, b)) return false; cx = a; cy = b; break;
        case 'H': if (!open) return fail("H before M"); if (!detail::num(d, k, a)) return fail("bad H"); if (!addPt(sh, a, cy)) return false; cx = a; break;
        case 'V': if (!open) return fail("V before M"); if (!detail::num(d, k, b)) return fail("bad V"); if (!addPt(sh, cx, b)) return false; cy = b; break;
        case 'Q': if (!open) return fail("Q before M");
          if (!detail::num(d, k, x1) || !detail::num(d, k, y1) || !detail::num(d, k, a) || !detail::num(d, k, b)) return fail("bad Q");
          for (int t = 1; t <= 8; ++t) { float u = t / 8.f, v = 1 - u; if (!addPt(sh, v * v * cx + 2 * v * u * x1 + u * u * a, v * v * cy + 2 * v * u * y1 + u * u * b)) return false; }
          cx = a; cy = b; break;
        default: return fail("path data without a command");
      }
    }
    return true;
  }
  bool parse(Doc& doc) {
    if (s.size() > kMaxInput) return fail("SVG too large");
    bool inSvg = false, done = false; std::string_view pendingClose;
    while (true) {
      while (i < s.size() && s[i] != '<') { if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') return fail("text content outside the subset"); ++i; }
      if (i >= s.size()) break;
      if (done) return fail("content after </svg>");
      ++i;
      if (i + 1 < s.size() && s[i] == '?') { size_t e = s.find("?>", i); if (e == std::string_view::npos || inSvg) return fail("bad XML declaration"); i = e + 2; continue; }
      if (i < s.size() && s[i] == '/') { ++i; std::string_view n; if (!name(n)) return fail("bad closing tag"); ws(); if (i >= s.size() || s[i] != '>') return fail("bad closing tag"); ++i;
        if (!pendingClose.empty()) { if (n != pendingClose) return fail("mismatched closing tag"); pendingClose = {}; continue; }
        if (n == "svg") { if (!inSvg) return fail("</svg> without <svg>"); inSvg = false; done = true; } else return fail("closing tag without its element");
        continue; }
      if (!pendingClose.empty()) return fail("shapes cannot nest");
      std::string_view tag; if (!name(tag)) return fail("bad tag");
      const bool isSvg = tag == "svg";
      if (isSvg == inSvg) return fail(isSvg ? "nested <svg>" : "shape outside <svg>");
      Shape sh; float tx = 0, ty = 0, ksx = 1, ksy = 1; float g[6] = {0, 0, 0, 0, 0, 0}; uint32_t have = 0;
      std::string_view d, points;
      if (!isSvg) { sh.fill.on = true; }  // SVG default fill is black
      bool selfClose = false;
      while (true) {
        ws(); if (i >= s.size()) return fail("unterminated tag");
        if (s[i] == '>') { ++i; break; }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>') { i += 2; selfClose = true; break; }
        std::string_view an; if (!name(an)) return fail("bad attribute name"); ws();
        if (i >= s.size() || s[i] != '=') return fail("attribute without value"); ++i; ws();
        if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return fail("unquoted attribute"); char q = s[i++];
        size_t e = s.find(q, i); if (e == std::string_view::npos) return fail("unterminated attribute"); std::string_view v = s.substr(i, e - i); i = e + 1;
        auto f = [&](int slot) { if (!detail::unit(v, g[slot])) return fail("bad number attribute"); have |= 1u << slot; return true; };
        if (an == "xmlns") { if (!isSvg) return fail("xmlns on a shape"); }
        else if (isSvg && an == "viewBox") { size_t k = 0; if (!detail::num(v, k, doc.vbX) || !detail::num(v, k, doc.vbY) || !detail::num(v, k, doc.vbW) || !detail::num(v, k, doc.vbH) || doc.vbW <= 0 || doc.vbH <= 0) return fail("bad viewBox"); }
        else if (isSvg && an == "width") { if (!detail::unit(v, doc.width) || doc.width <= 0) return fail("bad width"); }
        else if (isSvg && an == "height") { if (!detail::unit(v, doc.height) || doc.height <= 0) return fail("bad height"); }
        else if (isSvg) return fail("svg attribute outside the subset");
        else if (an == "fill") { if (!detail::paint(v, sh.fill)) return fail("bad fill"); }
        else if (an == "stroke") { if (!detail::paint(v, sh.stroke)) return fail("bad stroke"); }
        else if (an == "fill-opacity") { if (!detail::frac(v, sh.fill.a)) return fail("bad fill-opacity"); }
        else if (an == "stroke-opacity") { if (!detail::frac(v, sh.stroke.a)) return fail("bad stroke-opacity"); }
        else if (an == "opacity") { if (!detail::frac(v, sh.opacity)) return fail("bad opacity"); }
        else if (an == "stroke-width") { if (!detail::unit(v, sh.strokeWidth) || sh.strokeWidth < 0 || sh.strokeWidth > 1024) return fail("bad stroke-width"); }
        else if (an == "stroke-linejoin") { if (v == "miter") sh.join = 0; else if (v == "round") sh.join = 1; else if (v == "bevel") sh.join = 2; else return fail("bad stroke-linejoin"); }
        else if (an == "stroke-linecap") { if (v == "butt") sh.cap = 0; else if (v == "round") sh.cap = 1; else if (v == "square") sh.cap = 2; else return fail("bad stroke-linecap"); }
        else if (an == "transform") {
          auto fn = [&](std::string_view& rest, std::string_view f, float& a, float& b, bool& two) {
            while (!rest.empty() && detail::isSp(rest[0])) rest.remove_prefix(1);
            if (rest.substr(0, f.size()) != f) return false; rest.remove_prefix(f.size());
            size_t close = rest.find(')'); if (close == std::string_view::npos) return false;
            std::string_view a1 = rest.substr(0, close); rest.remove_prefix(close + 1);
            size_t k = 0; if (!detail::num(a1, k, a)) return false; two = detail::num(a1, k, b);
            while (k < a1.size() && detail::isSp(a1[k])) ++k; return k == a1.size(); };
          std::string_view rest = v; bool two = false;
          if (!fn(rest, "translate(", tx, ty, two)) return fail("transform outside the subset (translate [scale] only)");
          if (!two) ty = 0;
          while (!rest.empty() && detail::isSp(rest[0])) rest.remove_prefix(1);
          if (!rest.empty()) { if (!fn(rest, "scale(", ksx, ksy, two)) return fail("transform outside the subset (translate [scale] only)"); if (!two) ksy = ksx;
            while (!rest.empty() && detail::isSp(rest[0])) rest.remove_prefix(1); if (!rest.empty() || ksx == 0 || ksy == 0) return fail("bad scale"); } }
        else if (tag == "path" && an == "d") d = v;
        else if (tag == "polygon" && an == "points") points = v;
        else if ((tag == "line" && an == "x1") || (tag == "circle" && an == "cx") || (tag == "rect" && an == "x")) { if (!f(0)) return false; }
        else if ((tag == "line" && an == "y1") || (tag == "circle" && an == "cy") || (tag == "rect" && an == "y")) { if (!f(1)) return false; }
        else if ((tag == "line" && an == "x2") || (tag == "circle" && an == "r") || (tag == "rect" && an == "width")) { if (!f(2)) return false; }
        else if ((tag == "line" && an == "y2") || (tag == "rect" && an == "height")) { if (!f(3)) return false; }
        else return fail("attribute outside the subset");
      }
      if (isSvg) { if (selfClose) return fail("empty <svg/>"); inSvg = true; if (doc.vbW <= 0 && (doc.width <= 0 || doc.height <= 0)) return fail("svg has no size"); if (doc.vbW <= 0) { doc.vbW = doc.width; doc.vbH = doc.height; } continue; }
      if (doc.shapes.size() >= kMaxShapes) return fail("too many shapes");
      if (tag == "path") { if (d.empty()) return fail("path without d"); if (!path(d, sh)) return false; }
      else if (tag == "polygon") { size_t k = 0; float a, b; if (!startContour(sh)) return false;
        while (true) { while (k < points.size() && detail::isSp(points[k])) ++k; if (k >= points.size()) break; if (!detail::num(points, k, a) || !detail::num(points, k, b)) return fail("bad polygon points"); if (!addPt(sh, a, b)) return false; }
        if (sh.pts.size() < 3) return fail("polygon with < 3 points"); sh.contourClosed.back() = 1; }
      else if (tag == "line") { if (have != 0xF) return fail("line needs x1 y1 x2 y2"); if (!startContour(sh) || !addPt(sh, g[0], g[1]) || !addPt(sh, g[2], g[3])) return false; sh.fill.on = false; }
      else if (tag == "circle") { if ((have & 7) != 7 || g[2] <= 0) return fail("circle needs cx cy r > 0"); if (!startContour(sh)) return false;
        for (int t = 0; t < 32; ++t) { const float an = float(t) * 0.19634954f; float c = 1, sn = 0; { float x = an, x2 = x * x; sn = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110))))); c = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132))))); }
          if (!addPt(sh, g[0] + g[2] * c, g[1] + g[2] * sn)) return false; } sh.contourClosed.back() = 1; }
      else if (tag == "rect") { if ((have & 0xC) != 0xC || g[2] <= 0 || g[3] <= 0) return fail("rect needs width/height > 0"); if (!startContour(sh) || !addPt(sh, g[0], g[1]) || !addPt(sh, g[0] + g[2], g[1]) || !addPt(sh, g[0] + g[2], g[1] + g[3]) || !addPt(sh, g[0], g[1] + g[3])) return false; sh.contourClosed.back() = 1; }
      else return fail("element outside the subset");
      for (auto& p : sh.pts) { p.x = p.x * ksx + tx; p.y = p.y * ksy + ty; }
      sh.strokeWidth *= (ksx < 0 ? -ksx : ksx);
      doc.shapes.push_back(std::move(sh));
      if (!selfClose) pendingClose = tag;
    }
    if (!done) return fail("missing </svg>");
    return true;
  }
};

/// Parse `bytes` into `out`. Returns false with *err set on any violation; out is then empty.
inline bool parse(const uint8_t* bytes, size_t n, Doc& out, const char** err) {
  out = Doc{};
  Parser p; p.s = std::string_view(reinterpret_cast<const char*>(bytes), n);
  Doc d; if (!p.parse(d)) { if (err) *err = p.err ? p.err : "SVG parse failed"; return false; }
  out = std::move(d); return true;
}

}  // namespace litholattice::lksvg
