// In-house JSON parser implementation. See json.h.
#include "litholattice/ui/json.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lithogui {

namespace {

class Parser {
public:
    Parser(const std::string& text) : s_(text) {}

    bool parse(JsonValue& out, std::string& error) {
        skipWs();
        if (!parseValue(out, 0)) { error = err_; return false; }
        skipWs();
        if (pos_ != s_.size()) { error = "trailing characters after JSON value"; return false; }
        return true;
    }

private:
    // Nesting cap (round-2 review SUP2-7, re-pinned): the check in
    // parseValue compares the value's depth (root value = 0, each
    // enclosing container adds one) against kMaxDepth + 1, so the
    // recorded contract holds: a document nested 64 wrapping levels
    // around an innermost value (deepest value at depth 65) parses,
    // and 66 fails. The cap itself is a stack defense; both sides
    // of the boundary are pinned by tests.
    static constexpr int kMaxDepth = 64;
    const std::string& s_;
    std::size_t pos_ = 0;
    std::string err_;

    bool fail(const std::string& msg) { err_ = msg + " at offset " + std::to_string(pos_); return false; }

    void skipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool expect(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth + 1) return fail("nesting too deep");
        skipWs();
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        char c = s_[pos_];
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.str);
        }
        if (c == 't') return parseLiteral("true", JsonValue::Type::Bool, true, out);
        if (c == 'f') return parseLiteral("false", JsonValue::Type::Bool, false, out);
        if (c == 'n') return parseLiteral("null", JsonValue::Type::Null, false, out);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return fail("unexpected character");
    }

    bool parseLiteral(const char* word, JsonValue::Type type, bool b, JsonValue& out) {
        std::size_t len = std::strlen(word);
        if (s_.compare(pos_, len, word) != 0) return fail("invalid literal");
        pos_ += len;
        out.type = type;
        out.boolean = b;
        return true;
    }

    bool parseNumber(JsonValue& out) {
        // Strict JSON number grammar (review SUP-6):
        //   -? (0 | [1-9][0-9]*) ('.' [0-9]+)? ([eE] [+-]? [0-9]+)?
        // No leading zeros, a digit is required on both sides of
        // '.', and exponent digits are required. The value must be
        // finite: strtod range errors (ERANGE / HUGE_VAL) are parse
        // errors, not silent infinities.
        std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9')
            return fail("invalid number");
        if (s_[pos_] == '0') {
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
                return fail("leading zeros are not valid JSON numbers");
        } else {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9')
                return fail("digits required after the decimal point");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9')
                return fail("digits required in the exponent");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        errno = 0;
        out.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        // Range policy (round-2 review SUP2-2): only TRUE overflow
        // is a parse error. strtod reports overflow as +/-HUGE_VAL
        // (infinity on IEEE targets) and sets ERANGE; it ALSO sets
        // ERANGE for underflow to a subnormal or to zero (1e-999,
        // 1e-320, 4.9e-324), which are valid JSON numbers whose
        // values are perfectly representable: those are accepted
        // with the value strtod returned. (Round 1's ERANGE check
        // rejected both kinds and killed whole theme documents over
        // one harmless underflowing token.)
        if (std::isinf(out.number))
            return fail("number out of range");
        out.type = JsonValue::Type::Number;
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (!expect('"')) return fail("expected string");
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return fail("truncated escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // High surrogate: combine ONLY with an
                            // immediately following \u low surrogate
                            // (review SUP-2). Anything else makes this
                            // escape one U+FFFD, and the following
                            // escape (if any) is parsed as its own
                            // escape next iteration: never consumed,
                            // never emitted as a raw surrogate.
                            if (pos_ + 1 < s_.size() && s_[pos_] == '\\' &&
                                s_[pos_ + 1] == 'u') {
                                std::size_t savePos = pos_;
                                pos_ += 2;
                                unsigned lo = 0;
                                if (!parseHex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) +
                                         (lo - 0xDC00);
                                } else {
                                    pos_ = savePos; // not a pair: reparse it
                                    cp = 0xFFFD;
                                }
                            } else {
                                cp = 0xFFFD;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            cp = 0xFFFD; // lone low surrogate
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return fail("invalid escape");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("unescaped control character in string");
            } else {
                out += c;
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        out.array.clear();
        ++pos_; // '['
        skipWs();
        if (expect(']')) return true;
        while (true) {
            JsonValue item;
            if (!parseValue(item, depth + 1)) return false;
            out.array.push_back(std::move(item));
            skipWs();
            if (expect(',')) continue;
            if (expect(']')) return true;
            return fail("expected ',' or ']' in array");
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        out.object.clear();
        ++pos_; // '{'
        skipWs();
        if (expect('}')) return true;
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (!expect(':')) return fail("expected ':' in object");
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skipWs();
            if (expect(',')) continue;
            if (expect('}')) return true;
            return fail("expected ',' or '}' in object");
        }
    }
};

} // namespace

bool parseJson(const std::string& text, JsonValue& out, std::string& error) {
    Parser p(text);
    return p.parse(out, error);
}

} // namespace lithogui
