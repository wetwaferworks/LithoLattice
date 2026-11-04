#include "litholattice/core/json.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace litholattice {
namespace {
struct Parser {
  const std::string& s; size_t i = 0; std::string err;
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
  bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
  bool hex4(std::uint32_t& cp) {
    if (s.size() - i < 4) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = s[i + size_t(k)];
      cp <<= 4;
      if (h >= '0' && h <= '9') cp |= std::uint32_t(h - '0');
      else if (h >= 'a' && h <= 'f') cp |= std::uint32_t(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') cp |= std::uint32_t(h - 'A' + 10);
      else return false;
    }
    i += 4;
    return true;
  }
  static void utf8(std::uint32_t cp, std::string& o) {
    if (cp < 0x80) { o.push_back(char(cp)); }
    else if (cp < 0x800) { o.push_back(char(0xC0 | (cp >> 6))); o.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { o.push_back(char(0xE0 | (cp >> 12))); o.push_back(char(0x80 | ((cp >> 6) & 0x3F))); o.push_back(char(0x80 | (cp & 0x3F))); }
    else { o.push_back(char(0xF0 | (cp >> 18))); o.push_back(char(0x80 | ((cp >> 12) & 0x3F))); o.push_back(char(0x80 | ((cp >> 6) & 0x3F))); o.push_back(char(0x80 | (cp & 0x3F))); }
  }
  bool str(std::string& o) {
    if (s[i] != '"') return fail("expected string");
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\' && i < s.size()) {
        const char e = s[i++];
        if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'r') c = '\r';
        else if (e == 'b') c = '\b'; else if (e == 'f') c = '\f';
        else if (e == 'u') {
          // H6: bounds + hex validation (never skip past the end / closing quote),
          // real code-point decoding so dump() output round-trips.
          std::uint32_t cp = 0;
          if (!hex4(cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            std::uint32_t lo = 0;
            const size_t save = i;
            if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
              i += 2;
              if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              else { i = save; cp = 0xFFFD; }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          utf8(cp, o);
          continue;
        } else c = e;
      }
      o.push_back(c);
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i; return true;
  }
  // Depth cap: crafted deeply-nested input must not overflow the stack (DoS).
  // 128 levels is far beyond any legitimate config/mod JSON.
  static constexpr int kMaxDepth = 128;
  bool val(Json& v, int depth = 0) {
    ws();
    if (i >= s.size()) return fail("unexpected end");
    const char c = s[i];
    if (c == '{') {
      if (depth >= kMaxDepth) return fail("nesting too deep");
      v.type = Json::Type::Object; ++i; ws();
      if (s[i] == '}') { ++i; return true; }
      for (;;) {
        ws(); std::string k; if (!str(k)) return false;
        ws(); if (s[i] != ':') return fail("expected ':'"); ++i;
        Json m; if (!val(m, depth + 1)) return false;
        v.obj.emplace_back(std::move(k), std::move(m));
        ws(); if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      if (depth >= kMaxDepth) return fail("nesting too deep");
      v.type = Json::Type::Array; ++i; ws();
      if (s[i] == ']') { ++i; return true; }
      for (;;) {
        Json m; if (!val(m, depth + 1)) return false;
        v.arr.push_back(std::move(m));
        ws(); if (s[i] == ',') { ++i; continue; }
        if (s[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { v.type = Json::Type::String; return str(v.str); }
    if (s.compare(i, 4, "true") == 0) { v.type = Json::Type::Bool; v.b = true; i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { v.type = Json::Type::Bool; i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
    // Numbers (review A-L3): validate the JSON number grammar FIRST instead
    // of handing the text straight to strtod, which also accepts spellings
    // JSON forbids — NaN, Infinity, and hex floats (0x1p4) all parsed as
    // numbers before, and dump() then silently rewrote them to null.
    // Grammar (RFC 8259): -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    {
      const size_t start = i;
      if (i < s.size() && s[i] == '-') ++i;
      if (i >= s.size()) return fail("bad number");
      if (s[i] == '0') {
        ++i;
      } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
      } else {
        return fail("bad number");
      }
      if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
      }
      if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
      }
      v.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
      // A grammar-valid literal whose magnitude overflows double would come
      // back non-finite (inf) — reject it rather than silently changing its
      // meaning to null on dump.
      if (!std::isfinite(v.num)) return fail("number out of range");
      v.type = Json::Type::Number;
      return true;
    }
  }
  // True when nothing but whitespace remains after the top-level value.
  bool atEnd() { ws(); return i >= s.size(); }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
  Parser p{text, 0, {}};
  out = Json{};
  bool ok = p.val(out);
  // Review A-L3: a successful top value followed by trailing garbage is NOT
  // valid JSON ("{\"a\":1} garbage" parsed true before). Require end-of-input
  // (modulo whitespace) after the top-level value.
  if (ok && !p.atEnd()) {
    p.err = "trailing input after top-level value";
    ok = false;
  }
  if (!ok && err) *err = p.err;
  return ok;
}

namespace {

void dumpString(const std::string& s, std::string& out) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void dumpNumber(double v, std::string& out) {
  if (!std::isfinite(v)) { out += "null"; return; }  // JSON has no NaN / infinity
  char buf[32];
  for (int precision = 6; precision <= 17; ++precision) {  // shortest text that reads back exactly
    std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  out += buf;
}

void dumpValue(const Json& j, int indent, int depth, std::string& out) {
  const auto newline = [&](int d) {
    if (indent < 0) return;
    out += '\n';
    out.append(size_t(indent) * size_t(d), ' ');
  };
  switch (j.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.b ? "true" : "false"; break;
    case Json::Type::Number: dumpNumber(j.num, out); break;
    case Json::Type::String: dumpString(j.str, out); break;
    case Json::Type::Array: {
      // Arrays of plain numbers stay on one line (points, vectors).
      const bool flat = std::all_of(j.arr.begin(), j.arr.end(), [](const Json& e) { return e.type == Json::Type::Number; });
      out += '[';
      for (size_t i = 0; i < j.arr.size(); ++i) {
        if (i) out += flat || indent < 0 ? ", " : ",";
        if (!flat) newline(depth + 1);
        dumpValue(j.arr[i], indent, depth + 1, out);
      }
      if (!flat && !j.arr.empty()) newline(depth);
      out += ']';
      break;
    }
    case Json::Type::Object:
      out += '{';
      for (size_t i = 0; i < j.obj.size(); ++i) {
        if (i) out += indent < 0 ? ", " : ",";
        newline(depth + 1);
        dumpString(j.obj[i].first, out);
        out += ": ";
        dumpValue(j.obj[i].second, indent, depth + 1, out);
      }
      if (!j.obj.empty()) newline(depth);
      out += '}';
      break;
  }
}

}  // namespace

std::string Json::dump(int indent) const {
  std::string out;
  dumpValue(*this, indent, 0, out);
  return out;
}

}  // namespace litholattice
