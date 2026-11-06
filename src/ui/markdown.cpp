// Litho GUI - markdown subset parser. See markdown.h for the
// supported subset and the fixed style choices. The parser is a
// line classifier (block level) over a recursive inline scanner;
// recursion is depth-capped so hostile nesting degrades to literal
// text instead of deep stacks.

#include "litholattice/ui/markdown.h"

#include <algorithm>
#include <map>
#include <vector>

namespace lithogui {
namespace {

constexpr int kMaxInlineDepth = 16;

TextStyle boldOf(TextStyle s) {
    s.weight = 700;
    return s;
}
TextStyle italicOf(TextStyle s) {
    s.slant = FontSlant::Italic;
    return s;
}
TextStyle codeOf(TextStyle s) {
    s.weight = 500;
    s.color = Color{0.72f, 0.55f, 1.0f, 1.0f}; // trace purple
    return s;
}
TextStyle linkOf(TextStyle s) {
    s.color = Color{0.35f, 0.65f, 1.0f, 1.0f}; // trace blue
    s.underline = true;
    return s;
}
TextStyle strikeOf(TextStyle s) {
    s.strikethrough = true;
    return s;
}
TextStyle quoteOf(TextStyle s) {
    if (s.color.a > 0.0f) {
        s.color.r *= 0.6f;
        s.color.g *= 0.6f;
        s.color.b *= 0.6f;
    } else {
        s.color = Color{0.55f, 0.58f, 0.68f, 1.0f};
    }
    return s;
}

bool escapable(char c) {
    return c == '*' || c == '_' || c == '`' || c == '~' || c == '\\' || c == '[';
}

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Appends the spans for one line's inline content to `doc`, styled
// from `style`. Plain text accumulates into a single run per style
// stretch; styled constructs flush it.
//
// Hostile-input shape (round-2 review MD-2): every closer search in
// this scanner runs FORWARD ONLY over the same string, so a search
// that reaches the end without finding its needle poisons every
// later search for that needle on this line: the dead* flags below
// record exactly that, and the link URL scan (MD-5's balanced
// parens) is answered from a one-pass paren index instead of a
// rescan per '['. The old per-'[' find(')') restart was quadratic:
// "[a](b" x 80000 (400 KB, no closing paren anywhere) took ~1.1 s.
void parseInline(const std::string& s, const TextStyle& style, MarkdownDoc& doc,
                 int depth) {
    std::string plain;
    auto flush = [&]() {
        if (!plain.empty()) {
            doc.spans.push_back({plain, style});
            plain.clear();
        }
    };
    const std::size_t n = s.size();
    const std::size_t npos = std::string::npos;
    bool deadTick = false, deadStar3 = false, deadStar2 = false;
    bool deadStar1 = false, deadTilde = false, deadMid = false;
    // Lazily built paren index for URL scans: balance[i] is the
    // paren balance of s[0..i); closeAt maps a balance level to the
    // sorted positions of ')' characters sitting at that level;
    // nextWs[i] is the first space/tab at or after i (n when none).
    // A URL scan from `from` then closes at the first ')' position
    // >= from whose level equals balance[from] (the sequential
    // balanced scan's matching close), unless whitespace intervenes.
    bool parenReady = false;
    std::vector<int> balance;
    std::map<int, std::vector<std::size_t>> closeAt;
    std::vector<std::size_t> nextWs;
    auto buildParenIndex = [&]() {
        if (parenReady) return;
        parenReady = true;
        balance.assign(n + 1, 0);
        for (std::size_t i = 0; i < n; ++i)
            balance[i + 1] = balance[i] + (s[i] == '(' ? 1 : (s[i] == ')' ? -1 : 0));
        for (std::size_t t = 0; t < n; ++t)
            if (s[t] == ')') closeAt[balance[t]].push_back(t);
        nextWs.assign(n + 1, n);
        for (std::size_t i = n; i-- > 0;)
            nextWs[i] = (s[i] == ' ' || s[i] == '\t') ? i : nextWs[i + 1];
    };
    auto findUrlClose = [&](std::size_t from) -> std::size_t {
        buildParenIndex();
        auto it = closeAt.find(balance[from]);
        if (it == closeAt.end()) return npos;
        const std::vector<std::size_t>& v = it->second;
        auto lb = std::lower_bound(v.begin(), v.end(), from);
        if (lb == v.end()) return npos;
        if (nextWs[from] < *lb) return npos; // whitespace ends the URL first
        return *lb;
    };
    auto findMid = [&](std::size_t from) -> std::size_t {
        if (deadMid) return npos;
        std::size_t mid = s.find("](", from);
        if (mid == npos) deadMid = true;
        return mid;
    };

    std::size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && escapable(s[i + 1])) {
            plain += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '`') {
            std::size_t j = deadTick ? npos : s.find('`', i + 1);
            if (j == npos) deadTick = true;
            if (j != npos && j > i + 1) {
                flush();
                doc.spans.push_back({s.substr(i + 1, j - i - 1), codeOf(style)});
                i = j + 1;
                continue;
            }
            plain += '`';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '*' && i + 2 < n && s[i + 1] == '*' &&
            s[i + 2] == '*') {
            std::size_t j = deadStar3 ? npos : s.find("***", i + 3);
            if (j == npos) deadStar3 = true;
            if (j != npos && j > i + 3) {
                flush();
                parseInline(s.substr(i + 3, j - i - 3), boldOf(italicOf(style)),
                            doc, depth + 1);
                i = j + 3;
                continue;
            }
            plain += '*';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '*' && i + 1 < n && s[i + 1] == '*') {
            std::size_t j = deadStar2 ? npos : s.find("**", i + 2);
            if (j == npos) deadStar2 = true;
            if (j != npos && j > i + 2) {
                flush();
                parseInline(s.substr(i + 2, j - i - 2), boldOf(style), doc,
                            depth + 1);
                i = j + 2;
                continue;
            }
            plain += '*';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '~' && i + 1 < n && s[i + 1] == '~') {
            std::size_t j = deadTilde ? npos : s.find("~~", i + 2);
            if (j == npos) deadTilde = true;
            if (j != npos && j > i + 2) {
                flush();
                parseInline(s.substr(i + 2, j - i - 2), strikeOf(style), doc,
                            depth + 1);
                i = j + 2;
                continue;
            }
            plain += '~';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '_') {
            // Word-boundary rule for '_' (round-2 review MD-3, the
            // CommonMark intraword rule reduced to word characters):
            // an opener may not follow a word character and a closer
            // may not precede one, so identifiers like
            // my_file_name.txt keep their underscores literal. '*'
            // emphasis has no such rule (next branch).
            std::size_t close = npos;
            if (i == 0 || !isWordChar(s[i - 1])) {
                std::size_t j = i + 1;
                while (j < n) {
                    j = s.find('_', j);
                    if (j == npos) break;
                    if (j > i + 1 && (j + 1 >= n || !isWordChar(s[j + 1]))) {
                        close = j;
                        break;
                    }
                    ++j;
                }
            }
            if (close != npos) {
                flush();
                parseInline(s.substr(i + 1, close - i - 1), italicOf(style), doc,
                            depth + 1);
                i = close + 1;
                continue;
            }
            plain += '_';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '*') {
            std::size_t j = deadStar1 ? npos : s.find('*', i + 1);
            if (j == npos) deadStar1 = true;
            if (j != npos && j > i + 1) {
                flush();
                parseInline(s.substr(i + 1, j - i - 1), italicOf(style), doc,
                            depth + 1);
                i = j + 1;
                continue;
            }
            plain += '*';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '!' && i + 1 < n && s[i + 1] == '[') {
            // Image syntax is NOT supported and renders as literal
            // text (round-2 review MD-4): when the whole
            // ![...](...) construct is well-formed, consume it
            // verbatim so its bracket never becomes a live link on
            // the alt text. A malformed construct falls back to the
            // ordinary per-character path.
            std::size_t mid = findMid(i + 2);
            std::size_t close = (mid == npos) ? npos : findUrlClose(mid + 2);
            if (mid != npos && close != npos) { // empty alt still consumes
                plain += s.substr(i, close + 1 - i);
                i = close + 1;
                continue;
            }
            plain += '!';
            ++i;
            continue;
        }
        if (depth < kMaxInlineDepth && c == '[') {
            std::size_t mid = findMid(i + 1);
            std::size_t close = (mid == npos) ? npos : findUrlClose(mid + 2);
            if (mid != npos && close != npos && mid > i + 1) {
                flush();
                std::string url = s.substr(mid + 2, close - mid - 2);
                std::size_t firstSpan = doc.spans.size();
                parseInline(s.substr(i + 1, mid - i - 1), linkOf(style), doc,
                            depth + 1);
                for (std::size_t k = firstSpan; k < doc.spans.size(); ++k)
                    doc.links.push_back({k, url});
                i = close + 1;
                continue;
            }
            plain += '[';
            ++i;
            continue;
        }
        plain += c;
        ++i;
    }
    flush();
}

bool isBlankLine(const std::string& line) {
    for (char c : line)
        if (c != ' ' && c != '\t') return false;
    return true;
}

} // namespace

MarkdownDoc parseMarkdown(const std::string& text, const TextStyle& base) {
    MarkdownDoc doc;
    // Split into lines (a trailing '\r' per line is dropped so
    // CRLF sources behave like LF ones).
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    static const float kHeadingScale[6] = {1.6f, 1.45f, 1.32f,
                                           1.22f, 1.13f, 1.05f};
    bool haveContent = false;
    bool blankPending = false;
    for (const std::string& line : lines) {
        if (isBlankLine(line)) {
            if (haveContent) blankPending = true;
            continue;
        }
        if (haveContent)
            doc.spans.push_back({blankPending ? "\n\n" : "\n", base});
        haveContent = true;
        blankPending = false;

        // Heading: 1..6 '#' followed by a space (or end of line).
        std::size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#') ++hashes;
        if (hashes >= 1 && hashes <= 6 &&
            (hashes == line.size() || line[hashes] == ' ')) {
            TextStyle hs = base;
            hs.size = base.size * kHeadingScale[hashes - 1];
            hs.weight = 700;
            std::string content =
                hashes == line.size() ? std::string() : line.substr(hashes + 1);
            parseInline(content, hs, doc, 0);
            continue;
        }
        // Bullet: "- " or "* " prefix, content follows inline.
        if (line.size() >= 2 && (line[0] == '-' || line[0] == '*') &&
            line[1] == ' ') {
            doc.spans.push_back({"\xE2\x80\xA2  ", base}); // "•  "
            parseInline(line.substr(2), base, doc, 0);
            continue;
        }
        // Quote: "> " (or a bare ">") prefix, content muted.
        if (line[0] == '>' && (line.size() == 1 || line[1] == ' ')) {
            std::string content =
                line.size() <= 2 ? std::string() : line.substr(2);
            parseInline(content, quoteOf(base), doc, 0);
            continue;
        }
        parseInline(line, base, doc, 0);
    }
    return doc;
}

// ------------------------------------------------------- Label glue

void Label::setMarkdown(const std::string& md) {
    // The widget's own textStyle override when it has one, else a
    // default TextStyle (palette colors still substitute at draw
    // time for spans whose resolved color keeps alpha 0).
    setMarkdown(md, textStyle ? *textStyle : TextStyle{});
}

void Label::setMarkdown(const std::string& md, const TextStyle& base) {
    MarkdownDoc doc = parseMarkdown(md, base);
    spans_ = doc.spans;
    mdLinks_.clear();
    for (const MarkdownLink& lk : doc.links)
        mdLinks_.push_back({lk.spanIndex, lk.url});
}

} // namespace lithogui
