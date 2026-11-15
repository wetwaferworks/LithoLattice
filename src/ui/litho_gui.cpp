// Litho GUI base layer - implementation. See litho_gui.h for the design.
#include "litholattice/ui/litho_gui.h"

#include "litholattice/ui/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace lithogui {

const Style& defaultStyle() {
    static const Style s{};
    return s;
}

Style::Style() {
    titleText.size = 15.0f;
    titleText.weight = 600;
}

Style validatedStyle(Style s) {
    // The safety floor for a directly-set Style (review C6). Every
    // metric below feeds a division or a float->int cast somewhere
    // in layout / draw / pointer mapping (rowHeight in the row
    // widgets, lineHeightMul in ITextMetrics::lineHeight, text
    // sizes in both); theme values get the same treatment inside
    // applyThemeJson. Anything not listed here is either guarded at
    // its use site (uiScale via scaleFactor) or cannot produce UB.
    auto badMetric = [](float v) { return !std::isfinite(v) || v <= 0.0f; };
    if (badMetric(s.rowHeight)) s.rowHeight = 22.0f;
    if (badMetric(s.titleBarHeight)) s.titleBarHeight = 28.0f;
    if (!std::isfinite(s.borderWidth) || s.borderWidth < 0.0f) s.borderWidth = 1.0f;
    if (!std::isfinite(s.cornerRadius) || s.cornerRadius < 0.0f) s.cornerRadius = 4.0f;
    if (!std::isfinite(s.defaultPadding) || s.defaultPadding < 0.0f) s.defaultPadding = 8.0f;
    if (badMetric(s.bodyText.size)) s.bodyText.size = 14.0f;
    if (badMetric(s.titleText.size)) s.titleText.size = 15.0f;
    if (badMetric(s.bodyText.lineHeightMul)) s.bodyText.lineHeightMul = 1.2f;
    if (badMetric(s.titleText.lineHeightMul)) s.titleText.lineHeightMul = 1.2f;
    if (!std::isfinite(s.bodyText.letterSpacing)) s.bodyText.letterSpacing = 0.0f;
    if (!std::isfinite(s.titleText.letterSpacing)) s.titleText.letterSpacing = 0.0f;
    if (!std::isfinite(s.tracePhase)) s.tracePhase = 0.0f;
    if (!std::isfinite(s.tracePhaseSpeed)) s.tracePhaseSpeed = 1.0f;
    auto clamp01 = [](float v, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    };
    s.uiOpacity = clamp01(s.uiOpacity, 1.0f);
    s.windowOpacity = clamp01(s.windowOpacity, 1.0f);
    s.traceOpacity = clamp01(s.traceOpacity, 1.0f);
    return s;
}

namespace {
// NaN-safe clamp (review CORE-M6): NaN fails every comparison, so
// the guard form returns the low bound for NaN instead of passing
// it through into layout math.
float clampf(float v, float lo, float hi) {
    if (!(v >= lo)) return lo;
    return v > hi ? hi : v;
}

// Rows of overscan the virtualized row widgets (VirtualList,
// TreeView, ListBox, windowed ScrollArea) lay out / draw beyond the
// visible window on each side, so a partially scrolled row never
// pops in unbound.
constexpr int kRowOverscan = 2;

// Closed rectangle outline as a 5-point polyline.
std::vector<Vec2> rectOutline(Rect r) {
    return {{r.x, r.y},
            {r.x + r.w, r.y},
            {r.x + r.w, r.y + r.h},
            {r.x, r.y + r.h},
            {r.x, r.y}};
}

Color scaledAlpha(Color c, float mul) {
    c.a *= mul;
    return c;
}

// Decodes one UTF-8 code point at byte offset i and advances i past it.
// Malformed bytes decode as U+FFFD and advance one byte. Well-formed
// sequences that encode an invalid scalar value (overlong encodings,
// UTF-16 surrogates, values above U+10FFFF) also decode as U+FFFD,
// consuming the whole sequence (review CORE-M10: the decoder
// normalizes, as this comment always promised).
char32_t nextCodePoint(const std::string& s, std::size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    int len = 0;
    char32_t cp = 0;
    char32_t minCp = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; minCp = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; minCp = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; minCp = 0x10000; }
    else { ++i; return 0xFFFD; }
    if (i + static_cast<std::size_t>(len) > s.size()) { ++i; return 0xFFFD; }
    for (int k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += static_cast<std::size_t>(len);
    if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0xFFFD;
    return cp;
}

// Encodes one code point as UTF-8, appending to out. Invalid values
// (surrogates, > U+10FFFF) encode as U+FFFD.
void utf8Encode(char32_t cp, std::string& out) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// The start of the code point that ends at or contains byte offset
// pos (review CORE-M10). Stepping back over continuation bytes only
// finds a candidate; malformed runs can make that candidate a false
// boundary, so the result is verified by decoding FORWARD from the
// candidate and returning the last true boundary before pos.
std::size_t prevCpBoundary(const std::string& s, std::size_t pos) {
    if (pos > s.size()) pos = s.size();
    if (pos == 0) return 0;
    std::size_t cand = pos - 1;
    while (cand > 0 && (static_cast<unsigned char>(s[cand]) & 0xC0) == 0x80) --cand;
    std::size_t i = cand, last = cand;
    while (i < pos) {
        std::size_t j = i;
        nextCodePoint(s, j);
        if (j <= i) break;
        last = i;
        i = j;
    }
    return last;
}

// The code point boundary after the code point starting at pos.
std::size_t nextCpBoundary(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) return s.size();
    std::size_t i = pos;
    nextCodePoint(s, i);
    return i;
}

// ---------------------------------------------------------- text layout
// Shared presentation helpers: they give the TextStyle wrap / ellipsis /
// alignment fields real behavior against the ITextMetrics contract.
// The ellipsis marker is the three-dot "..." the house copy uses.

// Shortens `text` with a trailing "..." until it fits maxW (measured).
// Returns the text unchanged when it already fits or maxW <= 0.
std::string ellipsizeText(const std::string& text, const TextStyle& ts,
                          const ITextMetrics& m, float maxW) {
    if (maxW <= 0.0f || m.measure(text, ts) <= maxW) return text;
    const std::string dots = "...";
    const float dotsW = m.measure(dots, ts);
    if (dotsW > maxW) return "";
    // One forward pass (review CORE-M11): accumulate per-code-point
    // advances and keep the longest whole-code-point prefix that
    // leaves room for the dots. The old loop re-measured the whole
    // shrinking string per popped BYTE: O(n^2), and it could split a
    // multi-byte code point in half.
    float acc = 0.0f;
    std::size_t end = 0;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        std::size_t cs = i;
        char32_t cp = nextCodePoint(text, i);
        (void)cs;
        float adv = m.glyphAdvance(cp, ts.font, ts.size, ts.weight);
        if (i < n) adv += ts.letterSpacing; // spacing lives between cps
        if (acc + adv + dotsW > maxW) break;
        acc += adv;
        end = i;
    }
    std::string out = text.substr(0, end) + dots;
    // Safety net for providers whose measure is not a pure advance
    // sum (fallback resolution): shrink by whole code points until
    // it truly fits. Normally zero iterations.
    while (end > 0 && m.measure(out, ts) > maxW) {
        end = prevCpBoundary(text, end);
        out = text.substr(0, end) + dots;
    }
    return out;
}

// Splits `text` into display lines for width maxW. With wrap == Word,
// words are packed greedily; a single word wider than the line is
// hard-broken by characters. With wrap == None the result is one line.
// When ellipsis == End, an over-wide single line is ellipsized, and a
// wrapped result that overflows maxLines (when > 0) has its last line
// ellipsized... callers pass maxLines 0 for unbounded.
std::vector<std::string> layoutTextLines(const std::string& text, const TextStyle& ts,
                                         const ITextMetrics& m, float maxW,
                                         int maxLines = 0) {
    if (maxW <= 0.0f) return {text};
    if (ts.wrap != TextWrap::Word) {
        if (ts.ellipsis == Ellipsis::End) return {ellipsizeText(text, ts, m, maxW)};
        return {text};
    }
    // Word wrap: tokenize on spaces (byte-level; text in the base is
    // ASCII/UTF-8-agnostic for splitting since spaces are 0x20).
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        if (sp > i) words.push_back(text.substr(i, sp - i));
        i = sp + 1;
    }
    std::vector<std::string> lines;
    std::string cur;
    auto hardBreak = [&](const std::string& word) {
        // Word alone is wider than a line: break it by code points
        // (never mid-sequence; a multi-byte character stays whole).
        std::string piece;
        std::size_t wi = 0;
        while (wi < word.size()) {
            std::size_t cs = wi;
            nextCodePoint(word, wi);
            std::string next = piece + word.substr(cs, wi - cs);
            if (!piece.empty() && m.measure(next, ts) > maxW) {
                lines.push_back(piece);
                piece = word.substr(cs, wi - cs);
            } else {
                piece = next;
            }
        }
        cur = piece;
    };
    for (const std::string& w : words) {
        if (cur.empty()) {
            if (m.measure(w, ts) <= maxW) cur = w;
            else hardBreak(w);
        } else {
            std::string joined = cur + " " + w;
            if (m.measure(joined, ts) <= maxW) cur = joined;
            else {
                lines.push_back(cur);
                cur.clear();
                if (m.measure(w, ts) <= maxW) cur = w;
                else hardBreak(w);
            }
        }
    }
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines) {
        lines.resize(static_cast<std::size_t>(maxLines));
        if (ts.ellipsis == Ellipsis::End) {
            // Content was dropped: shorten the last kept line until it
            // fits with a trailing "..." marker. Pops whole code
            // points (review CORE-M11), never bytes.
            const float dotsW = m.measure("...", ts);
            std::string s = lines.back();
            while (!s.empty() && m.measure(s, ts) + dotsW > maxW)
                s.resize(prevCpBoundary(s, s.size()));
            lines.back() = (dotsW <= maxW) ? s + "..." : "";
        }
    }
    return lines;
}

// ------------------------------------------------- rich-span wrapping
// The span counterpart of layoutTextLines: tokenizes the WHOLE span
// sequence into words globally (a word may cross a span boundary; it
// then keeps one piece per span so styles survive), packs words
// greedily by measured width, and hard-breaks an over-wide word by
// code points. A separator space belongs to the span it appeared in
// and is dropped when its word starts a new line. Lines keep their
// pieces in order; separator pieces are marked so the drawer can
// advance past them without emitting a run.
struct SpanPiece {
    std::string text;
    TextStyle style;
    bool separator = false;
    int spanIndex = -1; // source span in the caller's span list (-1: none)
};
struct SpanLine {
    std::vector<SpanPiece> pieces;
    float width = 0.0f;
};
struct SpanWord {
    std::vector<SpanPiece> pieces; // the word's styled pieces, in order
    float width = 0.0f;
    bool spaceBefore = false;
    TextStyle spaceStyle; // valid when spaceBefore
    float spaceWidth = 0.0f;
    // Forced line breaks before this word, from '\n' characters in
    // the span text (markdown block separators): 1 ends the current
    // line, 2 leaves a blank line (paragraph break), and so on.
    int breaksBefore = 0;
};

std::vector<SpanLine> layoutSpanLines(const std::vector<TextSpan>& spans,
                                      const TextStyle& base,
                                      const ITextMetrics& m, float maxW) {
    // Tokenize into words across span boundaries.
    std::vector<SpanWord> words;
    SpanWord cur;
    bool curOpen = false;
    bool pendingSpace = false;
    int pendingBreaks = 0;
    TextStyle pendingSpaceStyle = base;
    auto closeWord = [&]() {
        if (!curOpen) return;
        words.push_back(std::move(cur));
        cur = SpanWord{};
        curOpen = false;
    };
    for (std::size_t si = 0; si < spans.size(); ++si) {
        const TextSpan& span = spans[si];
        TextStyle st = span.style ? *span.style : base;
        std::size_t i = 0;
        while (i < span.text.size()) {
            if (span.text[i] == ' ') {
                closeWord();
                pendingSpace = true;
                pendingSpaceStyle = st;
                ++i;
                continue;
            }
            if (span.text[i] == '\n') {
                // Forced break: closes the word and is charged to
                // the NEXT word as breaksBefore (see SpanWord).
                closeWord();
                pendingSpace = false;
                ++pendingBreaks;
                ++i;
                continue;
            }
            std::size_t j = span.text.find_first_of(" \n", i);
            if (j == std::string::npos) j = span.text.size();
            std::string piece = span.text.substr(i, j - i);
            if (!curOpen) {
                curOpen = true;
                if (pendingBreaks > 0) {
                    if (!words.empty())
                        cur.breaksBefore = pendingBreaks;
                    pendingBreaks = 0;
                }
                if (pendingSpace && !words.empty()) {
                    cur.spaceBefore = true;
                    cur.spaceStyle = pendingSpaceStyle;
                    cur.spaceWidth = m.measure(" ", pendingSpaceStyle);
                }
                pendingSpace = false;
            }
            cur.width += m.measure(piece, st);
            cur.pieces.push_back({piece, st, false, static_cast<int>(si)});
            i = j;
        }
    }
    closeWord();

    if (maxW <= 0.0f) {
        // No width budget: no lines (review CORE-L5). Callers treat
        // an empty result as "nothing fits", like a zero-height clip.
        return {};
    }

    std::vector<SpanLine> lines;
    SpanLine line;
    auto flush = [&]() {
        lines.push_back(std::move(line));
        line = SpanLine{};
    };
    // Places as much of `w` as fits on the current line, splitting the
    // word's pieces by code points across as many lines as needed;
    // the remainder stays on the final (open) line.
    auto placeWord = [&](SpanWord& w) {
        std::vector<SpanPiece> rest = w.pieces;
        bool firstChunk = true;
        while (!rest.empty()) {
            // The separator before the word belongs to its first
            // chunk, and its width is charged BEFORE the chunk is
            // packed (review CORE-L5): the old order packed against
            // the pre-separator budget and overfilled by one space.
            if (firstChunk && w.spaceBefore && !line.pieces.empty()) {
                line.pieces.push_back({" ", w.spaceStyle, true});
                line.width += w.spaceWidth;
            }
            float avail = maxW - line.width;
            float chunkW = 0.0f;
            std::vector<SpanPiece> chunk;
            std::vector<SpanPiece> leftover;
            for (std::size_t pi = 0; pi < rest.size(); ++pi) {
                const SpanPiece& pc = rest[pi];
                float pcW = m.measure(pc.text, pc.style);
                if (chunkW + pcW <= avail) {
                    chunk.push_back(pc);
                    chunkW += pcW;
                    continue;
                }
                // Split this piece by code points at the fit point.
                std::string head, tail;
                float headW = 0.0f;
                std::size_t ci = 0;
                while (ci < pc.text.size()) {
                    std::size_t cs = ci;
                    char32_t cp = nextCodePoint(pc.text, ci);
                    (void)cp;
                    float cw = m.measure(pc.text.substr(cs, ci - cs), pc.style);
                    if (!head.empty() && chunkW + headW + cw > avail) {
                        tail = pc.text.substr(cs);
                        break;
                    }
                    head += pc.text.substr(cs, ci - cs);
                    headW += cw;
                }
                if (!head.empty()) {
                    chunk.push_back({head, pc.style, false, pc.spanIndex});
                    chunkW += headW;
                }
                if (!tail.empty())
                    leftover.push_back({tail, pc.style, false, pc.spanIndex});
                for (std::size_t pj = pi + 1; pj < rest.size(); ++pj)
                    leftover.push_back(rest[pj]);
                break;
            }
            firstChunk = false;
            for (auto& pc : chunk) line.pieces.push_back(pc);
            line.width += chunkW;
            rest = std::move(leftover);
            if (!rest.empty()) flush();
        }
    };
    for (SpanWord& w : words) {
        // Forced breaks end the current line outright (an extra
        // flush per additional break leaves blank lines).
        for (int b = 0; b < w.breaksBefore; ++b) flush();
        float add = w.width + ((w.spaceBefore && !line.pieces.empty()) ? w.spaceWidth : 0.0f);
        if (!line.pieces.empty() && line.width + add > maxW) flush();
        if (line.pieces.empty() && w.width > maxW) {
            placeWord(w); // over-wide word on an empty line: hard-break
        } else if (line.width + w.width +
                       ((w.spaceBefore && !line.pieces.empty()) ? w.spaceWidth : 0.0f) <=
                   maxW) {
            if (w.spaceBefore && !line.pieces.empty()) {
                line.pieces.push_back({" ", w.spaceStyle, true});
                line.width += w.spaceWidth;
            }
            for (auto& pc : w.pieces) line.pieces.push_back(pc);
            line.width += w.width;
        } else {
            placeWord(w); // fits nowhere whole: fill this line, continue
        }
    }
    if (!line.pieces.empty() || lines.empty()) flush();
    return lines;
}
} // namespace

// ------------------------------------------------------------------ fonts

FontId FontRegistry::registerFont(FontFace face) {
    // An explicit id that is already registered is rejected
    // (review CORE-L8): silently shadowing a face would make
    // find(id) ambiguous. FONT_INVALID is returned and nothing is
    // stored.
    if (face.id != FONT_INVALID && find(face.id) != nullptr) return FONT_INVALID;
    if (face.id == FONT_INVALID) {
        face.id = nextId_++;
    } else if (face.id >= nextId_) {
        nextId_ = face.id + 1;
    }
    FontId id = face.id;
    faces_.push_back(std::move(face));
    return id;
}

const FontFace* FontRegistry::find(FontId id) const {
    for (auto& f : faces_) if (f.id == id) return &f;
    return nullptr;
}

const FontFace* FontRegistry::find(const std::string& family, int weight, FontSlant slant) const {
    const FontFace* familyMatch = nullptr;
    for (auto& f : faces_) {
        if (f.family != family) continue;
        if (f.weight == weight && f.slant == slant) return &f;
        if (!familyMatch) familyMatch = &f;
    }
    return familyMatch;
}

float ApproxTextMetrics::measure(const std::string& text, const TextStyle& style) const {
    float total = 0.0f;
    std::size_t i = 0;
    std::size_t count = 0;
    while (i < text.size()) {
        char32_t cp = nextCodePoint(text, i);
        total += glyphAdvance(cp, style.font, style.size, style.weight);
        ++count;
    }
    if (count > 1) total += style.letterSpacing * static_cast<float>(count - 1);
    return total;
}

FontMetrics ApproxTextMetrics::fontMetrics(FontId, float size) const {
    return {size * 0.8f, size * 0.2f, size * 0.2f};
}

std::vector<ResolvedRun> resolveRuns(const std::string& text, const TextStyle& style,
                                     const ITextMetrics& metrics) {
    std::vector<ResolvedRun> runs;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t start = i;
        char32_t cp = nextCodePoint(text, i);
        FontId chosen = style.font;
        if (!metrics.hasGlyph(chosen, cp)) {
            for (FontId fb : style.fallbacks) {
                if (metrics.hasGlyph(fb, cp)) { chosen = fb; break; }
            }
        }
        std::string piece = text.substr(start, i - start);
        if (!runs.empty() && runs.back().font == chosen) runs.back().text += piece;
        else runs.push_back({piece, chosen});
    }
    return runs;
}

// --------------------------------------------------------------- DrawList

void DrawList::pushClip(Rect r) {
    DrawCmd c; c.kind = DrawCmd::Kind::PushClip; c.rect = r; cmds_.push_back(std::move(c));
}
void DrawList::popClip() {
    DrawCmd c; c.kind = DrawCmd::Kind::PopClip; cmds_.push_back(std::move(c));
}
void DrawList::fillRect(Rect r, Color col, float cornerRadius) {
    DrawCmd c; c.kind = DrawCmd::Kind::FillRect; c.rect = r; c.color = col;
    c.cornerRadius = cornerRadius; cmds_.push_back(std::move(c));
}
void DrawList::line(Vec2 a, Vec2 b, Color col, float thickness) {
    DrawCmd c; c.kind = DrawCmd::Kind::Line; c.color = col; c.thickness = thickness;
    c.points = {a, b}; cmds_.push_back(std::move(c));
}
void DrawList::polyline(const std::vector<Vec2>& pts, Color col, float thickness) {
    DrawCmd c; c.kind = DrawCmd::Kind::Polyline; c.color = col; c.thickness = thickness;
    c.points = pts; cmds_.push_back(std::move(c));
}
void DrawList::polylineRange(const Vec2* pts, std::size_t n, Color col, float thickness) {
    DrawCmd c; c.kind = DrawCmd::Kind::Polyline; c.color = col; c.thickness = thickness;
    if (pts && n) c.points.assign(pts, pts + n);
    cmds_.push_back(std::move(c));
}
void DrawList::textRun(Rect origin, const std::string& s, const TextStyle& style, Color color,
                       float baseline) {
    DrawCmd c; c.kind = DrawCmd::Kind::TextRun; c.rect = origin; c.text = s;
    c.fontId = style.font; c.fontSize = style.size; c.fontWeight = style.weight;
    c.fontSlant = static_cast<int>(style.slant); c.letterSpacing = style.letterSpacing;
    c.baseline = baseline; c.fallbacks = style.fallbacks; c.color = color;
    cmds_.push_back(std::move(c));
}
void DrawList::imageQuad(Rect r, int textureId, float alpha) {
    DrawCmd c; c.kind = DrawCmd::Kind::ImageQuad; c.rect = r; c.textureId = textureId;
    // The [0, 1] bound on command alpha is enforced HERE, at
    // emission (round-2 review T-4): any producer (Image's effective
    // opacity, a rich row's avatar, a direct caller) is clamped the
    // same way, NaN included.
    c.imageAlpha = (alpha >= 0.0f) ? (alpha > 1.0f ? 1.0f : alpha) : 0.0f;
    cmds_.push_back(std::move(c));
}

void DrawList::imageQuad(Rect r, int textureId, Color tint) {
    imageQuad(r, textureId, tint.a);
    cmds_.back().imageTint = tint;
}
void DrawList::glyphQuad(Rect dest, Rect uv, int atlasId, FontId font, Color color) {
    DrawCmd c; c.kind = DrawCmd::Kind::GlyphQuad; c.rect = dest; c.uvRect = uv;
    c.textureId = atlasId; c.fontId = font; c.color = color;
    cmds_.push_back(std::move(c));
}
void DrawList::custom(std::uint32_t tag, Rect r, float phase) {
    DrawCmd c; c.kind = DrawCmd::Kind::CustomDraw; c.tag = tag; c.rect = r; c.phase = phase;
    cmds_.push_back(std::move(c));
}
void expandGlyphRuns(DrawList& dl, const IGlyphSource& src, const ITextMetrics& metrics) {
    std::vector<DrawCmd> out;
    out.reserve(dl.commands().size());
    for (const DrawCmd& c : dl.commands()) {
        if (c.kind != DrawCmd::Kind::TextRun) { out.push_back(c); continue; }
        // Rebuild the run's style from the command payload and segment
        // it through the fallback chain, exactly as a back end would.
        TextStyle ts;
        ts.font = c.fontId;
        ts.size = c.fontSize;
        ts.weight = c.fontWeight;
        ts.slant = static_cast<FontSlant>(c.fontSlant);
        ts.letterSpacing = c.letterSpacing;
        ts.fallbacks = c.fallbacks;
        std::vector<ResolvedRun> runs = resolveRuns(c.text, ts, metrics);
        // Pass 1 (review SUP-4): verify FULL coverage with the
        // non-baking queries before any glyphQuad call. A run that
        // cannot be fully covered (missing glyph, or its distinct
        // new cells exceed the atlas capacity left) bakes NOTHING
        // and stays a TextRun; the old single pass baked cells for
        // the covered prefix of a doomed run and threw them away.
        bool covered = true;
        {
            std::vector<std::pair<char32_t, FontId>> fresh;
            for (const ResolvedRun& run : runs) {
                std::size_t i = 0;
                while (i < run.text.size() && covered) {
                    char32_t cp = nextCodePoint(run.text, i);
                    if (!src.glyphAvailable(cp, run.font, ts.size, ts.weight)) {
                        covered = false;
                        break;
                    }
                    if (src.glyphNeedsNewCell(cp, run.font, ts.size, ts.weight)) {
                        std::pair<char32_t, FontId> key{cp, run.font};
                        bool seen = false;
                        for (auto& k : fresh) if (k == key) { seen = true; break; }
                        if (!seen) fresh.push_back(key);
                    }
                }
                if (!covered) break;
            }
            if (covered &&
                static_cast<int>(fresh.size()) > src.freeGlyphCells())
                covered = false;
        }
        std::vector<DrawCmd> quads;
        float penX = c.rect.x;
        const float baselineY = c.rect.y + c.baseline;
        // Pass 2 bakes only when pass 1 proved full coverage; a
        // doomed run never reaches a glyphQuad call.
        for (const ResolvedRun& run : runs) {
            if (!covered) break;
            std::size_t i = 0;
            while (i < run.text.size()) {
                char32_t cp = nextCodePoint(run.text, i);
                Rect dest, uv;
                int atlas = -1;
                if (!src.glyphQuad(cp, run.font, ts.size, ts.weight, penX, baselineY,
                                   dest, uv, atlas)) {
                    covered = false;
                    break;
                }
                DrawCmd q;
                q.kind = DrawCmd::Kind::GlyphQuad;
                q.rect = dest;
                q.uvRect = uv;
                q.textureId = atlas;
                q.fontId = run.font;
                q.color = c.color;
                quads.push_back(std::move(q));
                penX += metrics.glyphAdvance(cp, run.font, ts.size, ts.weight) +
                        ts.letterSpacing;
            }
            if (!covered) break;
        }
        if (covered) {
            for (auto& q : quads) out.push_back(std::move(q));
        } else {
            out.push_back(c); // not fully covered: the run stays a run
        }
    }
    dl.swapCommands(out);
}

void DrawList::traceBorder(Rect r, const Style& style) {
    // Flat fallback first (every back end can draw it today), then the
    // CustomDraw hook carrying the full shader-border payload.
    polyline(rectOutline(r), scaledAlpha(style.traceBlue, style.traceOpacity),
             style.borderWidth);
    DrawCmd c; c.kind = DrawCmd::Kind::CustomDraw; c.tag = CUSTOM_TRACE_BORDER;
    c.rect = r; c.cornerRadius = style.cornerRadius; c.borderWidth = style.borderWidth;
    c.phase = style.tracePhase;
    c.color = scaledAlpha(style.traceRed, style.traceOpacity);
    c.traceA = style.traceRed; c.traceB = style.traceBlue; c.traceC = style.tracePurple;
    cmds_.push_back(std::move(c));
}

void DrawList::scale(float s) { transform(s, {0.0f, 0.0f}); }

void DrawList::absorb(DrawList& other) {
    cmds_.reserve(cmds_.size() + other.cmds_.size());
    for (DrawCmd& c : other.cmds_) cmds_.push_back(std::move(c));
    other.cmds_.clear();
}

void DrawList::transform(float s, Vec2 offset) {
    if (s == 1.0f && offset.x == 0.0f && offset.y == 0.0f) return;
    // A non-positive or non-finite factor collapses or poisons
    // every command (round-3 review FRM-10); both current
    // callers guard, and the API now refuses too.
    if (!(s > 0.0f) || !std::isfinite(s)) return;
    // Field audit of DrawCmd: the spatial fields are rect, points,
    // cornerRadius (FillRect, CustomDraw), thickness (Line, Polyline),
    // fontSize / letterSpacing / baseline (TextRun), borderWidth
    // (CustomDraw). Everything else is not a length: color channels,
    // imageAlpha (a dimensionless multiplier), text bytes,
    // fontId / textureId / tag (ids), fontWeight (a token),
    // fontSlant (a token), fallbacks (ids), and CustomDraw `phase`
    // (a dimensionless animation parameter). The switch below names
    // every Kind with NO default case on purpose: -Wswitch (in -Wall)
    // warns when a future Kind is added without a scaling decision.
    // The offset translates POSITIONS only (rect origins, points);
    // sizes and the audited metrics above are scaled, never offset.
    for (DrawCmd& c : cmds_) {
        c.rect.x = c.rect.x * s + offset.x;
        c.rect.y = c.rect.y * s + offset.y;
        c.rect.w *= s;
        c.rect.h *= s;
        for (Vec2& p : c.points) {
            p.x = p.x * s + offset.x;
            p.y = p.y * s + offset.y;
        }
        switch (c.kind) {
            case DrawCmd::Kind::PushClip: break; // rect only
            case DrawCmd::Kind::PopClip: break;  // no spatial payload
            case DrawCmd::Kind::FillRect:
                c.cornerRadius *= s;
                break;
            case DrawCmd::Kind::Line:
            case DrawCmd::Kind::Polyline:
                c.thickness *= s;
                break;
            case DrawCmd::Kind::TextRun:
                c.fontSize *= s;
                c.letterSpacing *= s;
                c.baseline *= s;
                break;
            case DrawCmd::Kind::ImageQuad: break; // rect only; imageAlpha passes through
            case DrawCmd::Kind::GlyphQuad:
                // rect scaled above; uvRect deliberately NOT scaled:
                // it is normalized atlas space, not a length.
                break;
            case DrawCmd::Kind::CustomDraw:
                c.cornerRadius *= s;
                c.borderWidth *= s;
                // phase deliberately NOT scaled: dimensionless.
                break;
        }
    }
}

// ---------------------------------------------------------------- Widget

Widget* Widget::addChild(std::unique_ptr<Widget> child) {
    // Null children and cycles are rejected (review CORE-L8): adding
    // this widget to itself, or adding an ancestor below its own
    // descendant, would make every tree walk loop forever. Rejection
    // returns nullptr and does NOT adopt the child.
    if (!child) return nullptr;
    for (const Widget* p = this; p != nullptr; p = p->parent_) {
        if (p == child.get()) return nullptr;
    }
    child->parent_ = this;
    children_.push_back(std::move(child));
    return children_.back().get();
}

Widget* Widget::findById(const std::string& id) {
    if (id_ == id) return this;
    for (auto& c : children_) {
        if (Widget* hit = c->findById(id)) return hit;
    }
    return nullptr;
}

Rect Widget::treeRect() const {
    Rect out = rect_;
    for (const Widget* p = parent_; p; p = p->parent_) {
        // Build 27: every ancestor is a Frame now; only an
        // ENGAGED frame transforms (an inert one is the identity,
        // exactly as a non-Frame ancestor was before the merger).
        if (const Frame* f = dynamic_cast<const Frame*>(p);
            f && f->frameEngaged()) {
            const Vec2 tl = f->toTree({out.x, out.y});
            const Vec2 br = f->toTree({out.x + out.w, out.y + out.h});
            out = {tl.x, tl.y, br.x - tl.x, br.y - tl.y};
        }
    }
    return out;
}

Rect Widget::innerRect() const {
    return {rect_.x + padding.l, rect_.y + padding.t,
            std::max(0.0f, rect_.w - padding.l - padding.r),
            std::max(0.0f, rect_.h - padding.t - padding.b)};
}

DrawParams Widget::localParams(const DrawParams& params) const {
    DrawParams p = params;
    if (opacityOverride >= 0.0f) p.opacity = params.opacity * opacityOverride;
    return p;
}

TextStyle Widget::effectiveTextStyle(const Style& style) const {
    TextStyle ts = textStyle ? *textStyle : style.bodyText;
    if (ts.color.a <= 0.0f) ts.color = style.text;
    // Build 19: alignment is the widget's own settings-field
    // value, never the Style's or the override's align field -
    // JD's defaults (center; left for the list family) and any
    // per-instance choice live in the state word, and every draw
    // path that reads ts.align reads the setting through here.
    ts.align = textAlign();
    return ts;
}

float Widget::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    if (children_.empty()) return style.rowHeight;
    float total = padding.t + padding.b;
    if (dir == LayoutDir::Column) {
        for (auto& c : children_) total += c->desiredHeight(style) + c->margin.t + c->margin.b;
    } else {
        float m = 0.0f;
        for (auto& c : children_) m = std::max(m, c->desiredHeight(style) + c->margin.t + c->margin.b);
        total += m;
    }
    return total;
}

float Widget::desiredWidth(const Style& style) const {
    if (fixedW >= 0.0f) return fixedW;
    if (children_.empty()) return 0.0f;
    float total = padding.l + padding.r;
    if (dir == LayoutDir::Row) {
        for (auto& c : children_) total += c->desiredWidth(style) + c->margin.l + c->margin.r;
    } else {
        float m = 0.0f;
        for (auto& c : children_) m = std::max(m, c->desiredWidth(style) + c->margin.l + c->margin.r);
        total += m;
    }
    return total;
}

void Widget::layout(Rect r, const Style& style) {
    noteStyle(style);
    rect_ = r;
    if (children_.empty()) return;
    Rect inner = innerRect();

    if (dir == LayoutDir::Column || dir == LayoutDir::Row) {
        const bool column = (dir == LayoutDir::Column);
        const float innerMain = column ? inner.h : inner.w;
        // Width hints before desired-size queries (review CORE-M1):
        // in a Column, a child's width is the inner width minus its
        // margins (or its fixed width); telling the child up front
        // lets width-dependent desired heights (wrapped Labels) be
        // right on the first pass instead of one layout stale.
        if (column) {
            for (auto& c : children_) {
                if (!c->visible() || c->layoutExempt) continue;
                float w = (c->fixedW >= 0.0f)
                              ? c->fixedW
                              : std::max(0.0f, inner.w - c->margin.l - c->margin.r);
                c->setDesiredWidthHint(w);
            }
        }
        float fixedTotal = 0.0f;
        float flexTotal = 0.0f;
        for (auto& c : children_) {
            if (!c->visible() || c->layoutExempt) continue;
            fixedTotal += column ? (c->margin.t + c->margin.b) : (c->margin.l + c->margin.r);
            if (c->flex > 0.0f) flexTotal += c->flex;
            else fixedTotal += column ? c->desiredHeight(style) : c->desiredWidth(style);
        }
        float leftover = std::max(0.0f, innerMain - fixedTotal);
        float cursor = column ? inner.y : inner.x;
        for (auto& c : children_) {
            if (!c->visible() || c->layoutExempt) {
                // Exempt chrome keeps its rect (its owner places
                // it); invisible children collapse as always.
                if (!c->visible()) c->layout({inner.x, inner.y, 0, 0}, style);
                continue;
            }
            float mBefore = column ? c->margin.t : c->margin.l;
            float mAfter = column ? c->margin.b : c->margin.r;
            float main;
            if (c->flex > 0.0f && flexTotal > 0.0f) main = leftover * (c->flex / flexTotal);
            else main = column ? c->desiredHeight(style) : c->desiredWidth(style);
            cursor += mBefore;
            Rect cr;
            if (column) {
                float w = (c->fixedW >= 0.0f) ? c->fixedW
                                              : std::max(0.0f, inner.w - c->margin.l - c->margin.r);
                cr = {inner.x + c->margin.l, cursor, w, main};
            } else {
                float h = (c->fixedH >= 0.0f) ? c->fixedH
                                               : std::max(0.0f, inner.h - c->margin.t - c->margin.b);
                cr = {cursor, inner.y + c->margin.t, main, h};
            }
            c->layout(cr, style);
            cursor += main + mAfter;
        }
        return;
    }

    // LayoutDir::None: absolute placement with anchoring. The FIRST
    // layout places each child from its design inputs (position from
    // margins, size from fixed size or the inner rect minus margins).
    // Later layouts keep the placed rects: the container's origin
    // motion carries every child rigidly, and inner-size deltas are
    // applied per the child's ANCHOR word (see ANCHOR_* in the header).
    auto designRect = [](const Widget& c, const Rect& in) {
        return Rect{in.x + c.margin.l, in.y + c.margin.t,
                    (c.fixedW >= 0.0f) ? c.fixedW
                                       : std::max(0.0f, in.w - c.margin.l - c.margin.r),
                    (c.fixedH >= 0.0f) ? c.fixedH
                                       : std::max(0.0f, in.h - c.margin.t - c.margin.b)};
    };
    // A zero-size inner rect carries no placement information
    // (review CORE-H7): priming anchor state from it would measure
    // every later delta against a degenerate origin. Treat a layout
    // whose predecessor was zero-size (or which is itself the first)
    // as a fresh prime at design rects. Degenerate means EITHER
    // axis at zero (round-2 review C12: the AND form let a 400x0
    // first layout prime permanently with a 0-height baseline,
    // because priming itself accepts a rect with one live axis).
    const bool zeroPrime = !hasState(WS_ABS_PRIMED) ||
                           (lastInner_.w <= 0.0f || lastInner_.h <= 0.0f);
    if (zeroPrime) {
        for (auto& c : children_) {
            if (c->layoutExempt) continue; // owner-placed chrome
            c->layout(designRect(*c, inner), style);
            c->writeState(WS_ABS_PLACED, true);
        }
        writeState(WS_ABS_PRIMED, inner.w > 0.0f || inner.h > 0.0f);
    } else {
        const float odx = inner.x - lastInner_.x; // origin motion: rigid
        const float ody = inner.y - lastInner_.y;
        const float dx = inner.w - lastInner_.w;  // size delta: anchored
        const float dy = inner.h - lastInner_.h;
        for (auto& c : children_) {
            if (c->layoutExempt) continue; // owner-placed chrome
            if (!c->hasState(WS_ABS_PLACED)) { // child added after the first layout
                c->layout(designRect(*c, inner), style);
                c->writeState(WS_ABS_PLACED, true);
                continue;
            }
            Rect cr = c->rect();
            cr.x += odx;
            cr.y += ody;
            const std::uint32_t a = c->anchors;
            const bool L = (a & ANCHOR_LEFT) != 0u, R = (a & ANCHOR_RIGHT) != 0u;
            const bool T = (a & ANCHOR_TOP) != 0u, B = (a & ANCHOR_BOTTOM) != 0u;
            if (L && R) cr.w = std::max(0.0f, cr.w + dx);
            else if (R) cr.x += dx;
            else if (!L) cr.x += dx * 0.5f;
            if (T && B) cr.h = std::max(0.0f, cr.h + dy);
            else if (B) cr.y += dy;
            else if (!T) cr.y += dy * 0.5f;
            c->layout(cr, style);
        }
    }
    lastInner_ = inner;
}

void Widget::drawBackgroundAndBorder(DrawList& dl, const DrawParams& params) const {
    const Style& style = *params.style;
    Color bg = background(style);
    if (bg.a > 0.0f) dl.fillRect(rect_, params.fill(bg), style.cornerRadius);
    if (traceBorder()) dl.traceBorder(rect_, style);
}

void Widget::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    drawBackgroundAndBorder(dl, p);
    for (auto& c : children_) c->draw(dl, p);
}

// ----------------------------------------------------------------- Frame

void Frame::layout(Rect r, const Style& style) {
    // Inert frame (build 27): a widget that has not engaged its
    // Frame base lays out exactly as the widget always did - the
    // Widget container machinery, untouched. The viewport path
    // below runs only for engaged frames.
    if (!frameEngaged_) {
        Widget::layout(r, style);
        return;
    }
    noteStyle(style);
    // Animation overrides (build 14): while a box/scale override
    // stands (an animation running, or settled on its target),
    // the frame's box is the override, not the handed rect -
    // until the parent hands a rect that is neither the last
    // handed rect nor the frame's current effective box, which
    // means the parent genuinely moved or resized the frame:
    // the parent always wins, and the override and any running
    // animation drop with it. (An absolute parent re-hands a
    // child the child's own current rect on every relayout -
    // that is not a move, and must not kill a running animation:
    // the framed boot box lives under exactly such a parent.)
    if (hasLayoutArg_ &&
        (r.x != lastLayoutArg_.x || r.y != lastLayoutArg_.y ||
         r.w != lastLayoutArg_.w || r.h != lastLayoutArg_.h)) {
        const Rect eff = (animActive_ && animHasBox_) ? animBoxNow()
                           : hasBoxOverride_           ? boxOverride_
                                                       : rect_;
        if (r.x != eff.x || r.y != eff.y || r.w != eff.w || r.h != eff.h) {
            animActive_ = false;
            hasBoxOverride_ = false;
            scaleMul_ = 1.0f;
        }
    }
    lastLayoutArg_ = r;
    hasLayoutArg_ = true;
    const Rect box = (animActive_ && animHasBox_) ? animBoxNow()
                     : hasBoxOverride_            ? boxOverride_
                                                  : r;
    // Elastic mode (build 18): the design size follows the
    // effective box, so the fit below always lands at scale 1
    // with the content box equal to the frame's box (less the
    // pad). Done here, before the fit reads designW_/designH_,
    // and from the same box the fit uses, so an override-settled
    // elastic frame stays consistent too. Guarded like
    // setDesignSize: a degenerate box never poisons the design.
    if (elastic_) {
        if (std::isfinite(box.w) && box.w > 0.0f) designW_ = box.w;
        if (std::isfinite(box.h) && box.h > 0.0f) designH_ = box.h;
    }
    rect_ = box;
    // The fit: one uniform scale, the tighter axis governs, and
    // the scaled design area is centered in the frame's rect
    // (letterbox on the long axis). The fit runs against the
    // PADDED design size (design + 2*contentPad per axis,
    // build 13): the design area itself keeps its size and the
    // content box lands contentPad*scale inside the frame's
    // rect on every side, clear of the frame's own outline.
    // With the pad at its default 0 this is exactly the fit
    // every frame had before. A degenerate design size or
    // frame rect parks the content at zero scale rather than
    // dividing by it. The animation's scale multiplier (build
    // 14) multiplies the fit scale; at its default 1 the fit is
    // exactly the pre-animation fit.
    const float fitW = designW_ + 2.0f * contentPad_;
    const float fitH = designH_ + 2.0f * contentPad_;
    float s = 0.0f;
    if (fitW > 0.0f && fitH > 0.0f && box.w > 0.0f && box.h > 0.0f)
        s = std::min(box.w / fitW, box.h / fitH);
    if (!(s > 0.0f) || !std::isfinite(s)) s = 0.0f;
    s *= scaleMultiplier();
    scale_ = s;
    content_ = {box.x + (box.w - fitW * s) * 0.5f + contentPad_ * s,
                box.y + (box.h - fitH * s) * 0.5f + contentPad_ * s,
                designW_ * s, designH_ * s};
    if (children_.empty()) {
        contentH_ = designH_;
        if (scrollable()) clampScroll();
        return;
    }
    // Children live in DESIGN space: the design inner rect is
    // anchored at the design origin, and placement mirrors the
    // absolute container's rule (Widget::layout, LayoutDir::None)
    // measured against it instead of the tree-space inner rect:
    // prime from margins + fixed size (or the remaining space),
    // then carry origin motion rigidly and apply size deltas per
    // the child's anchor word. The design inner changes only
    // when the design size (or the frame padding) changes, so in
    // steady use the deltas are zero and the subtree's design
    // rects are stable across frame resizes - the frame's scale
    // absorbs every viewport change.
    //
    // SCROLLING (build 20): a scrollable Frame places its
    // children against the CONTENT inner - the design inner
    // grown to the settled content height - so bottom-anchored
    // children anchor to the content's bottom and fill children
    // fill the content. After the pass, the content height is
    // re-derived from the subtree's laid-out extent (measured,
    // never declared: children added, removed, resized or
    // re-expanded re-derive it on the next layout) and, when it
    // moved, a second pass applies the height change through
    // the same anchor machinery (a container resize, in effect)
    // and the scroll offset re-clamps. One settle pass always
    // converges: after it, bottom-anchored and fill children
    // end exactly at the content's inner bottom, so the
    // measured extent equals the settled height.
    Rect inner{padding.l, padding.t,
               std::max(0.0f, designW_ - padding.l - padding.r),
               std::max(0.0f, designH_ - padding.t - padding.b)};
    if (scrollable() && contentH_ > designH_)
        inner.h = std::max(0.0f, contentH_ - padding.t - padding.b);
    placeChildren(inner, style);
    if (scrollable()) {
        const float target =
            std::max(designH_, measureContentExtent() + padding.b);
        if (target != contentH_) {
            contentH_ = target;
            Rect settled = inner;
            settled.h = std::max(0.0f, target - padding.t - padding.b);
            placeChildren(settled, style);
        }
        clampScroll();
    } else {
        contentH_ = designH_;
    }
}

void Frame::placeChildren(const Rect& inner, const Style& style) {
    auto designRect = [](const Widget& c, const Rect& in) {
        return Rect{in.x + c.margin.l, in.y + c.margin.t,
                    (c.fixedW >= 0.0f) ? c.fixedW
                                       : std::max(0.0f, in.w - c.margin.l - c.margin.r),
                    (c.fixedH >= 0.0f) ? c.fixedH
                                       : std::max(0.0f, in.h - c.margin.t - c.margin.b)};
    };
    // Prune placement records for children no longer in the
    // tree (round-3 review FRM-8): pointer comparison only, the
    // recorded pointers are never dereferenced.
    auto prunePlaced = [this](std::vector<Widget*>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [this](const Widget* w) {
                                   for (auto& c : children_)
                                       if (c.get() == w) return false;
                                   return true;
                               }),
                v.end());
    };
    prunePlaced(placed_);
    prunePlaced(fillPlaced_);
    const bool zeroPrime = !hasState(WS_ABS_PRIMED) || lastDesignInner_.w <= 0.0f ||
                           lastDesignInner_.h <= 0.0f;
    if (zeroPrime) {
        for (auto& c : children_) {
            if (c->layoutExempt) continue; // owner-placed chrome
            c->layout(designRect(*c, inner), style);
            if (!wasPlaced(c.get())) {
                placed_.push_back(c.get());
                // A child primed without a fixed size onto the
                // whole design rect is a FILL child (FRM-3).
                if (c->fixedW < 0.0f && c->fixedH < 0.0f)
                    fillPlaced_.push_back(c.get());
            }
        }
        writeState(WS_ABS_PRIMED, inner.w > 0.0f || inner.h > 0.0f);
    } else {
        const float odx = inner.x - lastDesignInner_.x;
        const float ody = inner.y - lastDesignInner_.y;
        const float dx = inner.w - lastDesignInner_.w;
        const float dy = inner.h - lastDesignInner_.h;
        for (auto& c : children_) {
            if (c->layoutExempt) continue; // owner-placed chrome
            if (!wasPlaced(c.get())) { // child added after the first layout
                c->layout(designRect(*c, inner), style);
                placed_.push_back(c.get());
                if (c->fixedW < 0.0f && c->fixedH < 0.0f)
                    fillPlaced_.push_back(c.get());
                continue;
            }
            if (wasFillPlaced(c.get())) {
                // Fill children are re-pinned to the design rect
                // at every layout (round-3 review FRM-3): their
                // geometry belongs to the Frame, so a fill Window's
                // own title-drag or grip-resize cannot displace it
                // out of the frame and strand it, clipped and
                // unreachable, past the next layout. A Window
                // child needs its frame reset explicitly: a drag
                // calls Window::setFrame, after which
                // Window::layout ignores the rect it is handed.
                const Rect dr = designRect(*c, inner);
                if (auto* win = dynamic_cast<Window*>(c.get()))
                    win->setFrame(dr);
                c->layout(dr, style);
                continue;
            }
            Rect cr = c->rect();
            cr.x += odx;
            cr.y += ody;
            const std::uint32_t a = c->anchors;
            const bool L = (a & ANCHOR_LEFT) != 0u, R = (a & ANCHOR_RIGHT) != 0u;
            const bool T = (a & ANCHOR_TOP) != 0u, B = (a & ANCHOR_BOTTOM) != 0u;
            if (L && R) cr.w = std::max(0.0f, cr.w + dx);
            else if (R) cr.x += dx;
            else if (!L) cr.x += dx * 0.5f;
            if (T && B) cr.h = std::max(0.0f, cr.h + dy);
            else if (B) cr.y += dy;
            else if (!T) cr.y += dy * 0.5f;
            c->layout(cr, style);
        }
    }
    lastDesignInner_ = inner;
}

// ------------------------------------------------- Frame scrolling

void Frame::setScrollY(float y) {
    if (!std::isfinite(y)) return; // the last sane offset stands
    const float clamped = clampf(y, 0.0f, maxScrollY());
    const std::uint64_t q = static_cast<std::uint64_t>(
        clamped / kScrollYQuantum + 0.5f);
    if (q != loadField(kScrollYMask, kScrollYShift))
        storeField(kScrollYMask, kScrollYShift, q);
}

void Frame::clampScroll() {
    // Route the stored offset back through the setter: it
    // re-clamps against the settled content height and writes
    // only when the quantized value actually moved.
    setScrollY(scrollY());
}

namespace {
// The laid-out bottom edge of a subtree, in the Frame's
// content coordinates (children's rects are absolute in that
// space, so no offset accumulates down the walk). The walk
// does not descend into a widget that clips its children
// (ScrollArea, TreeView, ListBox, VirtualList, multi-line
// TextField) or into a nested Frame: those interiors are
// bounded by their own boxes, so the widget's own bottom is
// its whole contribution to the extent.
float subtreeExtent(const Widget* w) {
    float b = w->rect().y + w->rect().h + w->margin.b;
    // Build 27: the descent stops at a nested ENGAGED Frame (the
    // pre-merger "nested Frame"); inert widgets are descended
    // into exactly as every widget was before.
    const Frame* asFrame = dynamic_cast<const Frame*>(w);
    const bool engagedFrame = asFrame && asFrame->frameEngaged();
    if (!w->clipsChildren() && !engagedFrame) {
        for (auto& c : w->children()) {
            if (!c->visible()) continue;
            b = std::max(b, subtreeExtent(c.get()));
        }
    }
    return b;
}
} // namespace

float Frame::measureContentExtent() const {
    float e = 0.0f;
    for (auto& c : children_) {
        if (c->layoutExempt || !c->visible()) continue;
        e = std::max(e, subtreeExtent(c.get()));
    }
    return e;
}

void Frame::scrollThumbMetrics(float& top, float& h) const {
    const float trackH = designH_;
    const float ch = contentHeight();
    h = (ch > 0.0f) ? trackH * trackH / ch : trackH;
    // A minimum thumb keeps a barely-overflowing frame
    // grabbable; the cap keeps it inside its own track.
    const float minH = std::min(24.0f, trackH);
    if (h < minH) h = minH;
    if (h > trackH) h = trackH;
    const float travel = trackH - h;
    const float ms = maxScrollY();
    top = (travel > 0.0f && ms > 0.0f) ? travel * (scrollY() / ms) : 0.0f;
}

// ------------------------------------------------- Frame animation

void Frame::freezeAnimChannels() {
    // Settle whatever the running animation is currently showing
    // into the standing overrides, so a retarget (or a cancel)
    // starts from exactly what is on screen.
    if (!animActive_) return;
    if (animHasBox_) {
        boxOverride_ = animBoxNow();
        hasBoxOverride_ = true;
    }
    if (animHasScale_) scaleMul_ = animScaleNow();
    animSettledProgress_ = easedAnimT();
    animActive_ = false;
}

void Frame::beginAnim(Rect toBox, bool hasBox, float toScale, bool hasScale,
                      float durationSeconds, Easing easing) {
    freezeAnimChannels();
    animFromBox_ = hasBoxOverride_ ? boxOverride_ : rect_;
    animToBox_ = toBox;
    animHasBox_ = hasBox;
    animFromScale_ = scaleMul_;
    animToScale_ = toScale;
    animHasScale_ = hasScale;
    animElapsed_ = 0.0f;
    // A NaN or negative duration completes on the first tick.
    animDuration_ = (durationSeconds > 0.0f) ? durationSeconds : 0.0f;
    animEasing_ = easing;
    animSettledProgress_ = 0.0f;
    animActive_ = true;
    relayoutSelf(); // t = 0 geometry lands immediately
}

void Frame::animateBoxTo(Rect target, float durationSeconds, Easing easing) {
    beginAnim(target, true, scaleMul_, false, durationSeconds, easing);
}

void Frame::animateScaleTo(float targetScale, float durationSeconds, Easing easing) {
    const float sane = std::isfinite(targetScale) ? targetScale : 1.0f;
    beginAnim(Rect{}, false, sane, true, durationSeconds, easing);
}

void Frame::animateTo(Rect targetBox, float targetScale, float durationSeconds,
                      Easing easing) {
    const float sane = std::isfinite(targetScale) ? targetScale : 1.0f;
    beginAnim(targetBox, true, sane, true, durationSeconds, easing);
}

void Frame::cancelAnimation() {
    if (!animActive_) return;
    freezeAnimChannels(); // stand exactly where the freeze caught it
    relayoutSelf();
}

void Frame::tick(float dtSeconds) {
    if (!animActive_) return;
    const float dt = (dtSeconds > 0.0f) ? dtSeconds : 0.0f; // NaN/negative safe
    animElapsed_ += dt;
    if (animDuration_ <= 0.0f || animElapsed_ >= animDuration_) {
        // Settle exactly on the target, lay out there, and only
        // then fire the completion (the callback may rebuild the
        // world; it must see the final geometry).
        if (animHasBox_) {
            boxOverride_ = animToBox_;
            hasBoxOverride_ = true;
        }
        if (animHasScale_) scaleMul_ = animToScale_;
        animActive_ = false;
        animSettledProgress_ = 1.0f;
        relayoutSelf();
        if (onAnimationComplete) onAnimationComplete(*this);
        return;
    }
    relayoutSelf();
}

bool Frame::handlePointer(const PointerEvent& e) {
    // Inert frame (build 27): no scroll channels, no letterbox -
    // the widget's pre-merger answer (Widget's: unhandled).
    if (!frameEngaged_) return Widget::handlePointer(e);
    // Scroll channels (build 20). Coordinates here are the
    // Frame's PARENT space (dispatch maps into the subtree and
    // back out on ascent), the space content_ lives in. These
    // run before the letterbox path below: the wheel and the
    // scrollbar are the Frame's own business even though they
    // happen inside the content box - by the time an event
    // bubbles to the Frame, whatever sits under the pointer
    // has already had its claim (a self-scrolling child eats
    // its wheel first; the scrollbar strip was claimed for the
    // Frame by hit testing, so its press arrives here as the
    // target, not by theft from a sibling).
    if (scrollable() && scale_ > 0.0f) {
        if (e.type == PointerEvent::Type::Wheel) {
            // No overflow: decline, so an enclosing scrollable
            // frame (or any other ancestor) may still take the
            // notch. Outside the content box (the letterbox):
            // also decline - scrolling is for when we're inside.
            if (scrollOverflowing() && pointInContent(e.x, e.y)) {
                setScrollY(scrollY() - e.wheelDelta * kScrollWheelStep);
                return true;
            }
            return false;
        }
        if (e.type == PointerEvent::Type::Down && e.button == 0 &&
            scrollOverflowing() && pointInContent(e.x, e.y)) {
            const Vec2 d = toDesign({e.x, e.y});
            if (scrollbarHit(d.x, d.y)) {
                float tTop, tH;
                scrollThumbMetrics(tTop, tH);
                const float wy = d.y - scrollY(); // window-space y
                if (wy >= tTop && wy < tTop + tH) {
                    // Thumb press: the drag holds the grabbed
                    // point of the thumb under the cursor (the
                    // Move branch maps pointer travel through
                    // the thumb's own travel ratio).
                    raiseState(WS_DRAGGING);
                    dragStartY_ = e.y;
                    dragStartScroll_ = scrollY();
                    return true;
                }
                // Track press: page one visible height toward
                // the press. Capture is earned by the handled
                // Down; with no drag armed, the following Move
                // and Up simply fall through unhandled.
                setScrollY(scrollY() + (wy < tTop ? -designH_ : designH_));
                return true;
            }
        }
        if (e.type == PointerEvent::Type::Move && hasState(WS_DRAGGING)) {
            const float dDesign = (e.y - dragStartY_) / scale_;
            float tTop, tH;
            scrollThumbMetrics(tTop, tH);
            const float travel = designH_ - tH;
            // Grab-point invariant: one unit of thumb travel
            // (maxScroll over `travel` design units) per unit
            // of pointer travel; a full-height thumb (travel
            // 0, possible only on a sub-24-unit frame) drags
            // 1:1 in content units instead.
            const float factor =
                (travel > 0.0f) ? maxScrollY() / travel : 1.0f;
            setScrollY(dragStartScroll_ + dDesign * factor);
            return true;
        }
        if (e.type == PointerEvent::Type::Up && hasState(WS_DRAGGING)) {
            dropState(WS_DRAGGING);
            return true;
        }
    }
    // Only the letterbox case is the Frame's own business (see
    // the class comment): a press inside the content box was
    // already routed into the subtree by hit testing, and this
    // handler seeing it again on the bubble would double-deliver.
    // A letterbox press hit the Frame itself, so it is delivered,
    // mapped into design coordinates, through the dedicated
    // handleOutsidePress channel (round-3 review FRM-4/DEM-10):
    // Downs only (never moves, ups, or wheels), only to children
    // that explicitly override the channel, with the child list
    // snapshotted and every entry tombstone-checked - so a
    // letterbox press can never arm a Button, scroll an
    // unpointed ScrollArea, or walk a tree a handler destroyed.
    // At zero scale nothing is drawn, so nothing is live either
    // (FRM-5).
    if (e.type != PointerEvent::Type::Down) return false;
    if (scale_ <= 0.0f) return false;
    if (pointInContent(e.x, e.y)) return false;
    PointerEvent ev = e;
    const Vec2 d = toDesign({e.x, e.y});
    ev.x = d.x;
    ev.y = d.y;
    std::vector<Widget*> snapshot;
    snapshot.reserve(children_.size());
    for (auto& c : children_) snapshot.push_back(c.get());
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
        Widget* child = *it;
        if (widgetTombstoned(child) || widgetTombstoned(this)) return false;
        if (!child->visible() || !child->enabled()) continue;
        if (child->handleOutsidePress(ev)) return true;
        if (widgetTombstoned(this)) return false;
    }
    return false;
}

void Frame::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    // Inert frame (build 27): the widget's own face and children,
    // drawn the way the widget always drew them - no scratch-list
    // transform, no content clip.
    if (!frameEngaged_) {
        Widget::draw(dl, params);
        return;
    }
    DrawParams p = localParams(params);
    drawBackgroundAndBorder(dl, p); // own face (clear by default) + Trace outline
    if (!children_.empty() && scale_ > 0.0f) {
        // The subtree draws in design space into the retained
        // scratch list (round-3 review PERF-1: a fresh list here
        // grew and freed its buffer every frame; the member
        // keeps its capacity, the TreeView scratch_ pattern);
        // the transform lifts it into the content box, and the
        // clip keeps anything a child overdraws inside the
        // frame's outline. A scrollable Frame's transform origin
        // rides up by the scroll offset (build 20): the subtree
        // is drawn in content space, so the window shows the
        // slice starting at scrollY, and the clip - still the
        // visible box - decides what of it lands. Unscrolled,
        // the offset is 0 and the transform is the one the Frame
        // has always emitted.
        DrawList& sub = scratch_;
        sub.clear();
        for (auto& c : children_) c->draw(sub, p);
        sub.transform(scale_, {content_.x, content_.y - scrollOffset() * scale_});
        dl.pushClip(content_);
        dl.absorb(sub);
        dl.popClip();
    }
    // The overlay scrollbar (build 20): drawn in tree space,
    // over the content, at the content box's right edge - an
    // overlay, so no layout ever shifts when overflow appears
    // or disappears. Track and thumb in the house Trace style:
    // a quiet border-toned track, a trace-blue thumb, both
    // rounded to their width. It exists only while the content
    // overflows; the scrollable-without-overflow frame emits
    // exactly what it emitted before the property existed.
    if (scrollOverflowing() && scale_ > 0.0f) {
        const Style& style = *p.style;
        const float tw = kScrollTrackW * scale_;
        const Rect track{content_.x + content_.w - tw, content_.y, tw,
                         content_.h};
        float tTop, tH;
        scrollThumbMetrics(tTop, tH);
        const Rect thumb{track.x, content_.y + tTop * scale_, tw,
                         tH * scale_};
        Color trackCol = style.border;
        trackCol.a *= 0.35f;
        Color thumbCol = style.traceBlue;
        thumbCol.a *= 0.85f;
        dl.fillRect(track, p.fill(trackCol), tw * 0.5f);
        dl.fillRect(thumb, p.fill(thumbCol), tw * 0.5f);
    }
    // Debug frame outlines (JD, 2026-11-02): the shell's
    // bounding-box check. Loud colors that no theme token uses,
    // drawn OVER the content: magenta for the Frame's own box,
    // yellow for the scaled content box inside it. With the
    // flag off, nothing here runs and the list is the list the
    // Frame has always emitted.
    if (params.style && params.style->debugShowFrameOutlines) {
        dl.polyline(rectOutline(rect_), Color{1.0f, 0.0f, 1.0f, 1.0f}, 2.0f);
        dl.polyline(rectOutline(content_), Color{1.0f, 0.85f, 0.0f, 1.0f}, 2.0f);
    }
}

// ----------------------------------------------------------------- Label

Label::Label(std::string id, std::string text)
    : Frame(std::move(id), Frame::InertFrameTag{}), text_(std::move(text)) {}

void Label::layout(Rect r, const Style& style) {
    Widget::layout(r, style);
    lastLayoutW_ = rect_.w;
}

float Label::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    TextStyle ts = effectiveTextStyle(style);
    // The wrap width: the parent's hint when one was given for this
    // layout pass (review CORE-M1), else the width at last layout.
    const float wrapW = desiredWidthHint_ >= 0.0f ? desiredWidthHint_ : lastLayoutW_;
    if (!spans_.empty()) {
        if (ts.wrap == TextWrap::Word && wrapW > 0.0f) {
            // Same honest pattern as the plain-text path: the
            // approximate provider counts the wrapped span lines for
            // layout; the context provider governs drawing.
            static const ApproxTextMetrics approx;
            auto lines = layoutSpanLines(spans_, ts, approx,
                                         wrapW - 2.0f * textPad() - padding.l - padding.r);
            float lh = approx.lineHeight(ts);
            return static_cast<float>(lines.size()) * lh + 2.0f * textPad() +
                padding.t + padding.b;
        }
        return ts.size * ts.lineHeightMul;
    }
    if (ts.wrap == TextWrap::Word && wrapW > 0.0f) {
        // Measured with the approximate provider: the shipping provider
        // refines this, but line count is stable for layout purposes.
        static const ApproxTextMetrics approx;
        auto lines = layoutTextLines(text_, ts, approx,
                                     wrapW - 2.0f * textPad() - padding.l - padding.r);
        float lh = approx.lineHeight(ts);
        return static_cast<float>(lines.size()) * lh + 2.0f * textPad() +
                padding.t + padding.b;
    }
    return ts.size * ts.lineHeightMul;
}

void Label::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    // Cache the provider for the markdown link hit replay (review
    // MD-1): the replay must measure with the advances this draw
    // used, not with the approximate provider.
    lastMetrics_ = p.metrics;
    drawBackgroundAndBorder(dl, p);
    // Build 19 seating: runs lay out inside the padded box - the
    // textPad settings field plus this label's layout padding
    // (compositions use padding as an extra device on top of the
    // pad, e.g. the file browser's centering insets). Alignment
    // (the base style's align, which effectiveTextStyle takes
    // from the alignment setting) applies within this box.
    const float seatL = textPad() + padding.l;
    const float seatT = textPad() + padding.t;
    const float seatW = rect_.w - 2.0f * textPad() - padding.l - padding.r;
    auto emitRun = [&](const std::string& s, const TextStyle& ts, float x, float y) -> float {
        TextStyle t = ts;
        if (t.color.a <= 0.0f) t.color = style.text;
        // Text receives the effective opacity at emission (review
        // S3), exactly like fills (p.fill) and images (imageAlpha).
        Color col = p.fill(t.color);
        float w = metrics.measure(s, t);
        FontMetrics fm = metrics.fontMetrics(t.font, t.size);
        dl.textRun({x, y, w, t.size}, s, t, col, fm.ascent);
        // Style decorations (markdown links / strike spans): plain
        // line commands in the run's color, the same emission
        // pattern as the TextField preedit underline.
        if (t.underline)
            dl.line({x, y + fm.ascent + 2.0f}, {x + w, y + fm.ascent + 2.0f},
                    col, 1.0f);
        if (t.strikethrough) {
            float sy = y + fm.ascent * 0.62f;
            dl.line({x, sy}, {x + w, sy}, col, 1.0f);
        }
        return w;
    };
    if (!spans_.empty()) {
        TextStyle base = effectiveTextStyle(style);
        if (base.wrap == TextWrap::Word) {
            // Rich spans wrap across lines: words (possibly crossing
            // span boundaries) pack greedily, styles ride along per
            // piece, alignment applies per finished line.
            float availW = seatW;
            auto lines = layoutSpanLines(spans_, base, metrics, availW);
            float y = rect_.y + seatT;
            for (const SpanLine& line : lines) {
                float x = rect_.x + seatL;
                if (base.align == TextAlign::Center)
                    x += std::max(0.0f, (availW - line.width) * 0.5f);
                else if (base.align == TextAlign::Right)
                    x += std::max(0.0f, availW - line.width);
                float lineH = 0.0f;
                for (const SpanPiece& piece : line.pieces) {
                    lineH = std::max(lineH, metrics.lineHeight(piece.style));
                    if (piece.separator) {
                        x += metrics.measure(piece.text, piece.style);
                        continue;
                    }
                    x += emitRun(piece.text, piece.style, x, y);
                }
                y += lineH > 0.0f ? lineH : metrics.lineHeight(base);
            }
        } else {
            // Non-wrapping rich text honors padding, alignment and
            // ellipsis like the plain branch (review CORE-L5): the
            // old branch drew from the raw rect origin, unaligned
            // and unclipped, whatever the style asked for.
            const float availW = seatW;
            struct Piece { std::string text; TextStyle ts; float w; };
            std::vector<Piece> pieces;
            float total = 0.0f;
            for (auto& span : spans_) {
                TextStyle ts = span.style ? *span.style : base;
                if (ts.color.a <= 0.0f) ts.color = style.text;
                float w = metrics.measure(span.text, ts);
                pieces.push_back({span.text, ts, w});
                total += w;
            }
            if (base.ellipsis == Ellipsis::End && availW > 0.0f && total > availW) {
                // Truncate whole spans, then the boundary span by
                // code points (ellipsizeText), in that span's style.
                std::vector<Piece> kept;
                float keptW = 0.0f;
                for (auto& pc : pieces) {
                    if (keptW + pc.w <= availW) {
                        kept.push_back(pc);
                        keptW += pc.w;
                        continue;
                    }
                    std::string t = ellipsizeText(pc.text, pc.ts, metrics,
                                                  availW - keptW);
                    if (!t.empty())
                        kept.push_back({t, pc.ts, metrics.measure(t, pc.ts)});
                    break;
                }
                pieces = std::move(kept);
                total = 0.0f;
                for (auto& pc : pieces) total += pc.w;
            }
            float x = rect_.x + seatL;
            if (base.align == TextAlign::Center)
                x += std::max(0.0f, (availW - total) * 0.5f);
            else if (base.align == TextAlign::Right)
                x += std::max(0.0f, availW - total);
            const float y = rect_.y + seatT;
            for (auto& pc : pieces) x += emitRun(pc.text, pc.ts, x, y);
        }
    } else {
        TextStyle ts = effectiveTextStyle(style);
        float availW = seatW;
        auto lines = layoutTextLines(text_, ts, metrics, availW);
        float lineH = metrics.lineHeight(ts);
        float y = rect_.y + seatT;
        for (const std::string& line : lines) {
            float lw = metrics.measure(line, ts);
            float x = rect_.x + seatL;
            if (ts.align == TextAlign::Center) x += std::max(0.0f, (availW - lw) * 0.5f);
            else if (ts.align == TextAlign::Right) x += std::max(0.0f, availW - lw);
            emitRun(line, ts, x, y);
            y += lineH;
        }
    }
    for (auto& c : children_) c->draw(dl, p);
}

bool Label::handlePointer(const PointerEvent& e) {
    // Markdown link clicks: replay the layout draw used, measured
    // with the metrics provider the last draw used (cached in
    // lastMetrics_, review MD-1; the approximate provider only
    // before the first draw), and map the point back to a span.
    if (e.type != PointerEvent::Type::Up || e.button != 0 || !onLinkClick ||
        mdLinks_.empty())
        return false;
    static const ApproxTextMetrics approx;
    const ITextMetrics& metrics =
        lastMetrics_ ? *lastMetrics_ : static_cast<const ITextMetrics&>(approx);
    TextStyle base = effectiveTextStyle(lastStyle());
    // The replay seats exactly like draw (build 19 pad included).
    const float seatL = textPad() + padding.l;
    const float seatT = textPad() + padding.t;
    const float seatW = rect_.w - 2.0f * textPad() - padding.l - padding.r;
    const float availW = seatW;
    int hitSpan = -1;
    if (base.wrap == TextWrap::Word) {
        auto lines = layoutSpanLines(spans_, base, metrics, availW);
        float y = rect_.y + seatT;
        for (const SpanLine& line : lines) {
            float lineH = 0.0f;
            for (const SpanPiece& piece : line.pieces)
                lineH = std::max(lineH, metrics.lineHeight(piece.style));
            if (lineH <= 0.0f) lineH = metrics.lineHeight(base);
            if (e.y >= y && e.y < y + lineH) {
                float x = rect_.x + seatL;
                if (base.align == TextAlign::Center)
                    x += std::max(0.0f, (availW - line.width) * 0.5f);
                else if (base.align == TextAlign::Right)
                    x += std::max(0.0f, availW - line.width);
                for (const SpanPiece& piece : line.pieces) {
                    float w = metrics.measure(piece.text, piece.style);
                    if (!piece.separator && e.x >= x && e.x < x + w) {
                        hitSpan = piece.spanIndex;
                        break;
                    }
                    x += w;
                }
                break;
            }
            y += lineH;
        }
    } else {
        // One line of whole spans, in order (draw's non-wrapping
        // branch; ellipsis truncation is not replayed, so a click
        // past the truncation point simply misses).
        const float y = rect_.y + seatT;
        if (e.y >= y && e.y < y + metrics.lineHeight(base)) {
            std::vector<float> widths;
            float total = 0.0f;
            for (auto& span : spans_) {
                TextStyle ts = span.style ? *span.style : base;
                float w = metrics.measure(span.text, ts);
                widths.push_back(w);
                total += w;
            }
            float x = rect_.x + seatL;
            if (base.align == TextAlign::Center)
                x += std::max(0.0f, (availW - total) * 0.5f);
            else if (base.align == TextAlign::Right)
                x += std::max(0.0f, availW - total);
            for (std::size_t i = 0; i < spans_.size(); ++i) {
                if (e.x >= x && e.x < x + widths[i]) {
                    hitSpan = static_cast<int>(i);
                    break;
                }
                x += widths[i];
            }
        }
    }
    if (hitSpan >= 0)
        for (auto& lk : mdLinks_)
            if (static_cast<int>(lk.first) == hitSpan) {
                onLinkClick(*this, lk.second);
                return true;
            }
    return false;
}

// ---------------------------------------------------------------- Window

// ------------------------------------------------------------- TitleBar

bool TitleBar::handlePointer(const PointerEvent& e) {
    if (e.button != 0 || !owner_) return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            // The drag grab: remember where inside the owner the
            // press landed; moves keep that offset, exactly the
            // math the Window's own title drag always used.
            grabDx_ = e.x - owner_->rect().x;
            grabDy_ = e.y - owner_->rect().y;
            owner_->titleDragStart();
            return true;
        case PointerEvent::Type::Move:
            if (owner_->titleDragging()) {
                owner_->titleDragMoveTo(e.x - grabDx_, e.y - grabDy_);
                return true;
            }
            return false;
        case PointerEvent::Type::Up:
            if (owner_->titleDragging()) {
                owner_->titleDragEnd();
                return true;
            }
            return false;
        case PointerEvent::Type::Wheel:
            return false;
    }
    return false;
}

void TitleBar::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    dl.fillRect(rect_, p.fill(style.titleBar), style.cornerRadius);
    TextStyle ts = textStyle ? *textStyle : style.titleText;
    if (ts.color.a <= 0.0f) ts.color = style.text;
    // The title seats in the bar's padded box (build 19: the
    // textPad setting, which the build-14 kInnerPad / kTextInset
    // constants were unified into). It is ellipsized to the
    // padded width, so the text can never paint past the bar's
    // box or straddle the window's outline: what does not fit
    // ends in an ellipsis inside the bar instead of clipping at
    // its edge. Horizontally it anchors per the bar's alignment
    // setting - Center by JD's build-19 default; vertically it
    // centers in the padded box, which is the rect's own center
    // (the pad is symmetric).
    const Rect box = textRect();
    std::string shown = ellipsizeText(title_, ts, metrics, box.w);
    float tw = metrics.measure(shown, ts);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    dl.textRun({box.x + alignOffset(tw, box.w),
                rect_.y + (rect_.h - ts.size) * 0.5f, tw, ts.size},
               shown, ts, p.fill(ts.color), fm.ascent);
    for (auto& c : children_) c->draw(dl, p);
}

// ---------------------------------------------------------------- Window

Window::Window(std::string id, std::string title) : Frame(id, Frame::InertFrameTag{}) {
    setTraceBorder(true);
    // The composed TitleBar (build 14): the window's first child,
    // so it draws first among the children (where the window's
    // own title paint used to land) and hit testing finds it
    // across the top strip - under the grip band's precedence.
    auto bar = std::make_unique<TitleBar>(id + "-titlebar");
    bar->setTitle(std::move(title));
    bar->attachOwner(this);
    titleBar_ = bar.get();
    addChild(std::move(bar));
}

void Window::layout(Rect r, const Style& style) {
    if (hasFrame()) r = frame_;
    Widget::layout(r, style);
    // The TitleBar spans the window's top at the theme's title
    // height, laid out explicitly after the general child pass
    // (which primes it like any absolute child): title and
    // content below it scale as one anchored unit. Build 22: the
    // bar itself is inset by kTitleBarPad on the left, right,
    // and top (JD's item 1), so the bar's own fill and outline
    // stand clear of the window's trace outline instead of
    // riding on it.
    if (titleBar_) {
        if (textStyle) titleBar_->textStyle = textStyle;
        titleBar_->layout({rect_.x + kTitleBarPad, rect_.y + kTitleBarPad,
                           std::max(0.0f, rect_.w - 2.0f * kTitleBarPad),
                           TitleBar::heightFor(style)},
                          style);
    }
}

int Window::gripEdgesAt(float x, float y) const {
    if (!rect_.contains(x, y)) return 0;
    int edges = 0;
    if (x < rect_.x + kResizeGrip) edges |= EDGE_LEFT;
    if (x >= rect_.x + rect_.w - kResizeGrip) edges |= EDGE_RIGHT;
    if (y < rect_.y + kResizeGrip) edges |= EDGE_TOP;
    if (y >= rect_.y + rect_.h - kResizeGrip) edges |= EDGE_BOTTOM;
    return edges;
}

bool Window::handlePointer(const PointerEvent& e) {
    if (e.button != 0) return false;
    switch (e.type) {
        case PointerEvent::Type::Down: {
            // Resize zones win over the title drag: a grip-width band
            // inside each edge (corners take both axes). Hit testing
            // gives this handler precedence over children inside the
            // band (see hitRecurse), so a resize can start anywhere in
            // the band, even over a child that handles its own Down.
            int edges = gripEdgesAt(e.x, e.y);
            if (edges != 0) {
                resizeEdges_ = edges;
                dragStartFrame_ = hasFrame() ? frame_ : rect_;
                dragStartX_ = e.x;
                dragStartY_ = e.y;
                raiseState(WS_DRAGGING);
                return true;
            }
            // The title strip belongs to the composed TitleBar
            // (build 14): a press there that reached the Window
            // itself (hit testing normally delivers it straight
            // to the bar) starts the bar's drag.
            if (titleBar_ && titleBar_->rect().contains(e.x, e.y))
                return titleBar_->handlePointer(e);
            return false;
        }
        case PointerEvent::Type::Move: {
            if (resizeEdges_ != 0) {
                // Live resize: recompute the frame from the drag start
                // (never incrementally, so clamping cannot accumulate
                // error), keep the opposite edge planted, clamp to the
                // fixed minimum floor, and re-run layout so content
                // reflows during the drag, not on release.
                const float dx = e.x - dragStartX_;
                const float dy = e.y - dragStartY_;
                Rect f = dragStartFrame_;
                if (resizeEdges_ & EDGE_LEFT) {
                    f.x = dragStartFrame_.x + dx;
                    f.w = dragStartFrame_.w - dx;
                    if (f.w < kMinFrameW) {
                        f.w = kMinFrameW;
                        f.x = dragStartFrame_.x + dragStartFrame_.w - kMinFrameW;
                    }
                }
                if (resizeEdges_ & EDGE_RIGHT)
                    f.w = std::max(kMinFrameW, dragStartFrame_.w + dx);
                if (resizeEdges_ & EDGE_TOP) {
                    f.y = dragStartFrame_.y + dy;
                    f.h = dragStartFrame_.h - dy;
                    if (f.h < kMinFrameH) {
                        f.h = kMinFrameH;
                        f.y = dragStartFrame_.y + dragStartFrame_.h - kMinFrameH;
                    }
                }
                if (resizeEdges_ & EDGE_BOTTOM)
                    f.h = std::max(kMinFrameH, dragStartFrame_.h + dy);
                if (f.x != rect_.x || f.y != rect_.y || f.w != rect_.w || f.h != rect_.h) {
                    setFrame(f);
                    layout(f, lastStyle());
                    if (onResize) onResize(*this, f);
                }
                return true;
            }
            // A title drag in flight is the TitleBar's gesture;
            // forward moves that reach the Window (the bar holds
            // the grab offsets).
            if (hasState(WS_DRAGGING) && titleBar_)
                return titleBar_->handlePointer(e);
            return false;
        }
        case PointerEvent::Type::Up:
            if (resizeEdges_ != 0) {
                resizeEdges_ = 0;
                dropState(WS_DRAGGING);
                return true;
            }
            if (hasState(WS_DRAGGING) && titleBar_)
                return titleBar_->handlePointer(e);
            if (hasState(WS_DRAGGING)) { dropState(WS_DRAGGING); return true; }
            return false;
        case PointerEvent::Type::Wheel:
            return false;
    }
    return false;
}

void Window::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    // Windows without their own opacity override use the theme's
    // per-window default opacity.
    if (opacityOverride < 0.0f) p.opacity = params.opacity * style.windowOpacity;
    drawBackgroundAndBorder(dl, p);
    // The title bar is the composed TitleBar child (build 14): it
    // draws itself - bar fill and title text - as the first child.
    for (auto& c : children_) c->draw(dl, p);
}

// ---------------------------------------------------------------- Button

Button::Button(std::string id, std::string label)
    : Frame(std::move(id), Frame::InertFrameTag{}), label_(std::move(label)) {}

float Button::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    return style.rowHeight + 10.0f;
}

bool Button::handlePointer(const PointerEvent& e) {
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Down) { raiseState(WS_PRESSED); return true; }
    if (e.type == PointerEvent::Type::Up) {
        bool fire = hasState(WS_PRESSED) && rect_.contains(e.x, e.y);
        dropState(WS_PRESSED);
        if (fire && onClick) onClick(*this);
        return true;
    }
    return e.type == PointerEvent::Type::Move && hasState(WS_PRESSED);
}

bool Button::handleKey(const KeyEvent& e) {
    if (e.down && (e.key == KEY_ENTER || e.ch == U' ')) {
        if (onClick) onClick(*this);
        return true;
    }
    return false;
}

void Button::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    Color bg = (this == p.focused) ? style.controlBgHot : style.controlBg;
    dl.fillRect(rect_, p.fill(bg), style.cornerRadius);
    dl.polyline(rectOutline(rect_), (this == p.focused) ? style.tracePurple : style.border,
                style.borderWidth);
    TextStyle ts = effectiveTextStyle(style);
    // Button labels ellipsize to the padded box and anchor per
    // the alignment setting (build 19): Center by default, which
    // reproduces the button's historical centered label exactly -
    // pad + half the padded slack is half the total slack.
    const Rect box = textRect();
    std::string shown = ellipsizeText(label_, ts, metrics, box.w);
    float tw = metrics.measure(shown, ts);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    float tx = box.x + alignOffset(tw, box.w);
    float ty = rect_.y + (rect_.h - ts.size) * 0.5f;
    dl.textRun({tx, ty, tw, ts.size}, shown, ts,
               p.fill(enabled() ? ts.color : style.textMuted), fm.ascent);
    for (auto& c : children_) c->draw(dl, p);
}

// ---------------------------------------------------------------- Toggle

Toggle::Toggle(std::string id, std::string label, bool value)
    : Frame(std::move(id), Frame::InertFrameTag{}), label_(std::move(label)) {
    writeState(WS_ON, value);
}

void Toggle::flip() {
    flipState(WS_ON);
    if (onChange) onChange(*this, value());
}

float Toggle::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    // Tall enough for the content (the 18-high track, the text
    // line) to keep its pad on both sides (build 19).
    const float content =
        std::max(18.0f, style.bodyText.size * style.bodyText.lineHeightMul);
    return std::max(style.rowHeight, content + 2.0f * textPad());
}

bool Toggle::handlePointer(const PointerEvent& e) {
    // WS_PRESSED arming, like Button (review, suspected S1): the
    // toggle flips on Up only when the press STARTED on it, so a
    // drag that began elsewhere and releases here changes nothing.
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Down) { raiseState(WS_PRESSED); return true; }
    if (e.type == PointerEvent::Type::Up) {
        const bool was = hasState(WS_PRESSED);
        dropState(WS_PRESSED);
        if (was && rect_.contains(e.x, e.y)) flip();
        return was;
    }
    return false;
}

bool Toggle::handleKey(const KeyEvent& e) {
    if (e.down && (e.key == KEY_ENTER || e.ch == U' ')) { flip(); return true; }
    return false;
}

void Toggle::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const bool on = value();
    const float trackW = 36.0f, trackH = 18.0f;
    TextStyle ts = effectiveTextStyle(style);
    float tw = metrics.measure(label_, ts);
    // Build 19: the (track + gap + label) unit seats inside the
    // padded box, anchored as one unit per the alignment setting.
    // Build 22: the label starts at the label column - the
    // group's shared column when one is set (labelColumnW), else
    // this control's natural track + gap - so a composed group's
    // labels share one line across control widths.
    const Rect seat = textRect();
    const float labelCol = (labelColumnW >= 0.0f) ? labelColumnW : trackW + 8.0f;
    const float unitX = seat.x + alignOffset(labelCol + tw, seat.w);
    Rect track{unitX, rect_.y + (rect_.h - trackH) * 0.5f, trackW, trackH};
    dl.fillRect(track, p.fill(on ? style.traceBlue : style.controlBg), trackH * 0.5f);
    float knobR = trackH - 6.0f;
    float kx = on ? (track.x + trackW - knobR - 3.0f) : (track.x + 3.0f);
    dl.fillRect({kx, track.y + 3.0f, knobR, knobR}, style.text, knobR * 0.5f);
    if (this == p.focused) dl.polyline(rectOutline(rect_), style.tracePurple, style.borderWidth);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    dl.textRun({unitX + labelCol, rect_.y + (rect_.h - ts.size) * 0.5f, tw, ts.size},
               label_, ts, p.fill(enabled() ? ts.color : style.textMuted), fm.ascent);
    for (auto& c : children_) c->draw(dl, p);
}

// -------------------------------------------------------------- CheckBox

CheckBox::CheckBox(std::string id, std::string label, bool checked)
    : Frame(std::move(id), Frame::InertFrameTag{}), label_(std::move(label)) {
    writeState(WS_CHECKED, checked);
}

void CheckBox::flip() {
    flipState(WS_CHECKED);
    if (onChange) onChange(*this, checked());
}

float CheckBox::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    // Tall enough for the content (the 16 box, the text line) to
    // keep its pad on both sides (build 19).
    const float content =
        std::max(16.0f, style.bodyText.size * style.bodyText.lineHeightMul);
    return std::max(style.rowHeight, content + 2.0f * textPad());
}

bool CheckBox::handlePointer(const PointerEvent& e) {
    // WS_PRESSED arming, like Button (review, suspected S1).
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Down) { raiseState(WS_PRESSED); return true; }
    if (e.type == PointerEvent::Type::Up) {
        const bool was = hasState(WS_PRESSED);
        dropState(WS_PRESSED);
        if (was && rect_.contains(e.x, e.y)) flip();
        return was;
    }
    return false;
}

bool CheckBox::handleKey(const KeyEvent& e) {
    if (e.down && (e.key == KEY_ENTER || e.ch == U' ')) { flip(); return true; }
    return false;
}

void CheckBox::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const float boxS = 16.0f;
    TextStyle ts = effectiveTextStyle(style);
    float tw = metrics.measure(label_, ts);
    // Build 19: the (box + gap + label) unit seats inside the
    // padded box, anchored as one unit per the alignment setting.
    // Build 22: the label column, as on Toggle (see there).
    const Rect seat = textRect();
    const float labelCol = (labelColumnW >= 0.0f) ? labelColumnW : boxS + 8.0f;
    const float unitX = seat.x + alignOffset(labelCol + tw, seat.w);
    Rect box{unitX, rect_.y + (rect_.h - boxS) * 0.5f, boxS, boxS};
    dl.fillRect(box, p.fill(style.controlBg), 2.0f);
    dl.polyline(rectOutline(box), (this == p.focused) ? style.tracePurple : style.border,
                style.borderWidth);
    if (checked()) {
        dl.line({box.x + 3.0f, box.y + boxS * 0.55f}, {box.x + boxS * 0.42f, box.y + boxS - 3.0f},
                style.traceBlue, 2.0f);
        dl.line({box.x + boxS * 0.42f, box.y + boxS - 3.0f}, {box.x + boxS - 3.0f, box.y + 3.0f},
                style.traceBlue, 2.0f);
    }
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    dl.textRun({unitX + labelCol, rect_.y + (rect_.h - ts.size) * 0.5f, tw, ts.size},
               label_, ts, p.fill(enabled() ? ts.color : style.textMuted), fm.ascent);
    for (auto& c : children_) c->draw(dl, p);
}

// ----------------------------------------------------------- RadioButton

RadioButton::RadioButton(std::string id, std::string label, std::string groupId, int value)
    : Frame(std::move(id), Frame::InertFrameTag{}), label_(std::move(label)), groupId_(std::move(groupId)),
      value_(value) {}

void RadioButton::select() {
    // Re-selecting the already-selected button is not an event
    // (review CORE-L8): no sibling churn, no intent.
    if (selected()) return;
    if (parent_) {
        for (auto& sib : parent_->children()) {
            auto* rb = dynamic_cast<RadioButton*>(sib.get());
            if (rb && rb != this && rb->groupId_ == groupId_) rb->dropState(WS_SELECTED);
        }
    }
    raiseState(WS_SELECTED);
    if (onSelect) onSelect(*this);
}

float RadioButton::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    // Tall enough for the content (the 16 disc, the text line)
    // to keep its pad on both sides (build 19).
    const float content =
        std::max(16.0f, style.bodyText.size * style.bodyText.lineHeightMul);
    return std::max(style.rowHeight, content + 2.0f * textPad());
}

bool RadioButton::handlePointer(const PointerEvent& e) {
    // WS_PRESSED arming, like Button (review, suspected S1).
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Down) { raiseState(WS_PRESSED); return true; }
    if (e.type == PointerEvent::Type::Up) {
        const bool was = hasState(WS_PRESSED);
        dropState(WS_PRESSED);
        if (was && rect_.contains(e.x, e.y)) select();
        return was;
    }
    return false;
}

bool RadioButton::handleKey(const KeyEvent& e) {
    if (e.down && (e.key == KEY_ENTER || e.ch == U' ')) { select(); return true; }
    return false;
}

void RadioButton::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const float disc = 16.0f;
    TextStyle ts = effectiveTextStyle(style);
    float tw = metrics.measure(label_, ts);
    // Build 19: the (disc + gap + label) unit seats inside the
    // padded box, anchored as one unit per the alignment setting.
    // Build 22: the label column, as on Toggle (see there).
    const Rect seat = textRect();
    const float labelCol = (labelColumnW >= 0.0f) ? labelColumnW : disc + 8.0f;
    const float unitX = seat.x + alignOffset(labelCol + tw, seat.w);
    Rect outer{unitX, rect_.y + (rect_.h - disc) * 0.5f, disc, disc};
    dl.fillRect(outer, p.fill(style.controlBg), disc * 0.5f);
    dl.polyline(rectOutline(outer), (this == p.focused) ? style.tracePurple : style.border,
                style.borderWidth);
    if (selected()) {
        const float dot = 8.0f;
        dl.fillRect({outer.x + (disc - dot) * 0.5f, outer.y + (disc - dot) * 0.5f, dot, dot},
                    style.traceBlue, dot * 0.5f);
    }
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    dl.textRun({unitX + labelCol, rect_.y + (rect_.h - ts.size) * 0.5f, tw, ts.size},
               label_, ts, p.fill(enabled() ? ts.color : style.textMuted), fm.ascent);
    for (auto& c : children_) c->draw(dl, p);
}

// -------------------------------------------------------------- TreeView

void TreeView::walkNodes(std::vector<TreeNode>& nodes, int depth, int winFirst,
                         int winLast, int findIndex, int& count, Row* foundOut,
                         bool& found) const {
    for (auto& n : nodes) {
        if (found) return;
        if (findIndex >= 0 && count == findIndex) {
            found = true;
            if (foundOut) *foundOut = Row{&n, depth};
            return;
        }
        if (count >= winFirst && count < winLast) scratch_.push_back({&n, depth});
        ++count;
        if (n.expanded && !n.children.empty())
            walkNodes(n.children, depth + 1, winFirst, winLast, findIndex, count,
                      foundOut, found);
    }
}

int TreeView::flattenWindow(int winFirst, int winLast, int findIndex,
                            Row* foundOut) const {
    scratch_.clear(); // capacity retained: steady-state walks allocate nothing
    if (foundOut) *foundOut = Row{nullptr, 0};
    int count = 0;
    bool found = false;
    walkNodes(const_cast<std::vector<TreeNode>&>(roots_), 0, winFirst, winLast,
              findIndex, count, foundOut, found);
    lastMaterialized_ = static_cast<int>(scratch_.size());
    return count;
}

int TreeView::totalRowCount() const {
    return flattenWindow(0, 0, -1, nullptr); // count-only: stores nothing
}

int TreeView::firstVisibleIndex() const {
    if (rowH_ <= 0.0f) return 0;
    return std::min(totalRowCount(), static_cast<int>(scrollY_ / rowH_));
}

bool TreeView::handlePointer(const PointerEvent& e) {
    if (e.type == PointerEvent::Type::Wheel) {
        int total = flattenWindow(0, 0, -1, nullptr); // count-only walk
        float maxScr = std::max(0.0f, static_cast<float>(total) * rowH_ - rect_.h);
        scrollY_ = clampf(scrollY_ - e.wheelDelta * 24.0f, 0.0f, maxScr);
        return true;
    }
    if (e.type != PointerEvent::Type::Up || e.button != 0) {
        return e.type == PointerEvent::Type::Down && e.button == 0;
    }
    int idx = static_cast<int>((e.y - rect_.y + scrollY_) / rowH_);
    if (idx < 0) return true;
    // Resolve the clicked row by the counting walk: it stops at the
    // row and stores nothing.
    Row row{nullptr, 0};
    flattenWindow(0, 0, idx, &row);
    if (!row.node) return true; // past the last row
    float indent = static_cast<float>(row.depth) * 16.0f;
    // The expander test measures from the padded box's left edge:
    // draw seats row content there (build 19).
    float localX = e.x - rect_.x - textPad();
    bool onExpander = localX >= indent && localX < indent + 14.0f;
    if (onExpander && !row.node->children.empty()) {
        row.node->expanded = !row.node->expanded;
        if (onToggle) onToggle(*this, *row.node);
    } else {
        selectedId_ = row.node->id;
        if (onSelect) onSelect(*this, *row.node);
    }
    return true;
}

void TreeView::layout(Rect r, const Style& style) {
    rowH_ = style.rowHeight;
    Widget::layout(r, style);
}

void TreeView::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const_cast<TreeView*>(this)->rowH_ = style.rowHeight;
    drawBackgroundAndBorder(dl, p);
    dl.pushClip(rect_);
    // Window-only flatten, re-derived fresh on this draw: a
    // count-only walk gets the total (for the scroll clamp), then a
    // second walk stores ONLY the visible window plus overscan into
    // the reused scratch buffer. Rows outside the window are counted,
    // never stored, so bookkeeping stays O(window) at any tree size.
    int total = flattenWindow(0, 0, -1, nullptr);
    float maxScr = std::max(0.0f, static_cast<float>(total) * rowH_ - rect_.h);
    float sy = clampf(scrollY_, 0.0f, maxScr);
    int firstVis = rowH_ > 0.0f ? static_cast<int>(sy / rowH_) : 0;
    int first = std::max(0, firstVis - kRowOverscan);
    int visCount = rowH_ > 0.0f ? static_cast<int>(std::ceil(rect_.h / rowH_)) + 1 : 0;
    int last = std::min(total, firstVis + visCount + kRowOverscan);
    flattenWindow(first, last, -1, nullptr);
    const std::vector<Row>& rows = scratch_;
    TextStyle ts = effectiveTextStyle(style);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    int drawn = 0;
    for (int i = first; i < last; ++i) {
        const Row& row = rows[static_cast<std::size_t>(i - first)];
        float y = rect_.y + static_cast<float>(i) * rowH_ - sy;
        if (row.node->id == selectedId_)
            dl.fillRect({rect_.x, y, rect_.w, style.rowHeight}, p.fill(style.selection), 0.0f);
        float indent = static_cast<float>(row.depth) * 16.0f;
        // Row content seats inside the padded box horizontally
        // (build 19): the expander and label shift in by textPad.
        // The vertical row rhythm is the scroll surface's own -
        // rows tile the rect and scroll under its top and bottom
        // edges, so the pad binds the content's horizontal
        // seating here, not the tiling.
        float cx = rect_.x + textPad() + indent + 7.0f;
        float cy = y + style.rowHeight * 0.5f;
        if (!row.node->children.empty()) {
            if (row.node->expanded) {
                dl.line({cx - 4.0f, cy - 2.0f}, {cx + 4.0f, cy - 2.0f}, style.textMuted);
                dl.line({cx + 4.0f, cy - 2.0f}, {cx, cy + 4.0f}, style.textMuted);
                dl.line({cx, cy + 4.0f}, {cx - 4.0f, cy - 2.0f}, style.textMuted);
            } else {
                dl.line({cx - 2.0f, cy - 4.0f}, {cx - 2.0f, cy + 4.0f}, style.textMuted);
                dl.line({cx - 2.0f, cy + 4.0f}, {cx + 4.0f, cy}, style.textMuted);
                dl.line({cx + 4.0f, cy}, {cx - 2.0f, cy - 4.0f}, style.textMuted);
            }
        }
        float tw = metrics.measure(row.node->label, ts);
        dl.textRun({rect_.x + textPad() + indent + 18.0f,
                    y + (style.rowHeight - ts.size) * 0.5f, tw, ts.size},
                   row.node->label, ts, p.fill(ts.color), fm.ascent);
        ++drawn;
    }
    drawnRows_ = drawn;
    dl.popClip();
    for (auto& c : children_) c->draw(dl, p);
}

// --------------------------------------------------------------- ListBox

float ListBox::maxScroll() const {
    return std::max(0.0f, static_cast<float>(items_.size()) * rowH_ - rect_.h);
}

void ListBox::setScrollY(float y) {
    scrollY_ = clampf(y, 0.0f, maxScroll());
}

void ListBox::scrollThumbMetrics(float& top, float& h) const {
    const float trackH = rect_.h;
    const float contentH = static_cast<float>(items_.size()) * rowH_;
    h = (contentH > 0.0f) ? trackH * trackH / contentH : trackH;
    // The scrollable Frame's thumb discipline: a minimum keeps
    // a barely-overflowing list grabbable, the cap keeps the
    // thumb inside its own track.
    const float minH = std::min(24.0f, trackH);
    if (h < minH) h = minH;
    if (h > trackH) h = trackH;
    const float travel = trackH - h;
    const float ms = maxScroll();
    top = (travel > 0.0f && ms > 0.0f)
              ? travel * (clampf(scrollY_, 0.0f, ms) / ms)
              : 0.0f;
}

int ListBox::firstVisibleIndex() const {
    if (rowH_ <= 0.0f) return 0;
    return std::min(static_cast<int>(items_.size()), static_cast<int>(scrollY_ / rowH_));
}

bool ListBox::handlePointer(const PointerEvent& e) {
    if (e.type == PointerEvent::Type::Wheel) {
        scrollY_ = clampf(scrollY_ - e.wheelDelta * 24.0f, 0.0f, maxScroll());
        return true;
    }
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Down) {
        // Arm the gesture (build 23 drag scrolling): remember
        // the press point and the offset it landed on. A press
        // on the scrollbar strip is a scrollbar gesture: on the
        // thumb it arms a thumb drag (the grab point held under
        // the cursor); on the track it pages one visible height
        // toward the press and the gesture is spent, so the
        // following release selects nothing.
        pressActive_ = true;
        dragScrolled_ = false;
        pressScrollbar_ = false;
        pressY_ = e.y;
        pressScrollY_ = clampf(scrollY_, 0.0f, maxScroll());
        if (scrollbarHit(e.x)) {
            pressScrollbar_ = true;
            float tTop, tH;
            scrollThumbMetrics(tTop, tH);
            const float wy = e.y - rect_.y;
            if (wy >= tTop && wy < tTop + tH) {
                thumbGrabDy_ = wy - tTop;
                raiseState(WS_DRAGGING);
            } else {
                setScrollY(pressScrollY_ + (wy < tTop ? -rect_.h : rect_.h));
                dragScrolled_ = true; // a page is not a pick
            }
        }
        return true;
    }
    if (e.type == PointerEvent::Type::Move) {
        if (!pressActive_) return false;
        if (pressScrollbar_) {
            if (!hasState(WS_DRAGGING)) return true; // paged; spent
            float tTop, tH;
            scrollThumbMetrics(tTop, tH);
            const float travel = rect_.h - tH;
            const float ms = maxScroll();
            // Grab-point invariant, the scrollable Frame's own:
            // the thumb's top follows the pointer minus the
            // grab offset, and the offset follows the thumb's
            // share of its travel.
            if (travel > 0.0f && ms > 0.0f) {
                const float top = clampf(e.y - rect_.y - thumbGrabDy_, 0.0f, travel);
                setScrollY(ms * (top / travel));
            } else {
                setScrollY(pressScrollY_); // a full-height thumb cannot move
            }
            dragScrolled_ = true;
            return true;
        }
        // Content drag: past a small threshold the gesture is a
        // scroll - the content follows the finger - and the
        // release will select nothing. Below the threshold the
        // press is still a click in the making.
        const float dy = e.y - pressY_;
        if (dragScrolled_ || std::fabs(dy) > 4.0f) {
            dragScrolled_ = true;
            setScrollY(pressScrollY_ - dy);
        }
        return true;
    }
    if (e.type != PointerEvent::Type::Up) return false;
    const bool spent = dragScrolled_ || pressScrollbar_;
    pressActive_ = false;
    pressScrollbar_ = false;
    dragScrolled_ = false;
    dropState(WS_DRAGGING);
    if (spent) return true; // the gesture scrolled; it picks nothing
    int idx = static_cast<int>((e.y - rect_.y + scrollY_) / rowH_);
    if (idx >= 0 && idx < static_cast<int>(items_.size())) {
        selected_ = idx;
        if (onSelect) onSelect(*this, idx);
    }
    return true;
}

void ListBox::layout(Rect r, const Style& style) {
    rowH_ = style.rowHeight;
    Widget::layout(r, style);
}

void ListBox::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const_cast<ListBox*>(this)->rowH_ = style.rowHeight;
    drawBackgroundAndBorder(dl, p);
    dl.pushClip(rect_);
    // Windowed rows: visible slice plus overscan only.
    float sy = clampf(scrollY_, 0.0f,
                      std::max(0.0f, static_cast<float>(items_.size()) * rowH_ - rect_.h));
    int firstVis = rowH_ > 0.0f ? static_cast<int>(sy / rowH_) : 0;
    int first = std::max(0, firstVis - kRowOverscan);
    int visCount = rowH_ > 0.0f ? static_cast<int>(std::ceil(rect_.h / rowH_)) + 1 : 0;
    int last = std::min(static_cast<int>(items_.size()), firstVis + visCount + kRowOverscan);
    TextStyle ts = effectiveTextStyle(style);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    int drawn = 0;
    for (int i = first; i < last; ++i) {
        float y = rect_.y + static_cast<float>(i) * rowH_ - sy;
        if (i == selected_)
            dl.fillRect({rect_.x, y, rect_.w, style.rowHeight}, p.fill(style.selection), 0.0f);
        float tw = metrics.measure(items_[static_cast<std::size_t>(i)], ts);
        // Row text seats in the padded box, anchored per the
        // alignment setting (Left by the list family's class
        // default, build 19); the vertical rhythm is the scroll
        // surface's, as in TreeView.
        dl.textRun({rect_.x + textPad() + alignOffset(tw, rect_.w - 2.0f * textPad()),
                    y + (style.rowHeight - ts.size) * 0.5f, tw, ts.size},
                   items_[static_cast<std::size_t>(i)], ts, p.fill(ts.color), fm.ascent);
        ++drawn;
    }
    drawnRows_ = drawn;
    dl.popClip();
    // The scrollbar (build 23): an overlay at the right inside
    // edge, present only while the content overflows, in the
    // scrollable Frame's house style (a quiet border-toned
    // track, a trace-blue thumb). Before this build the list
    // scrolled by wheel alone and showed nothing of it - on a
    // touch surface there was no wheel and no sign the rest of
    // the list existed, which is half of what JD's dropdown
    // report was about.
    if (maxScroll() > 0.0f) {
        float tTop, tH;
        scrollThumbMetrics(tTop, tH);
        const Rect track{rect_.x + rect_.w - kScrollbarW, rect_.y,
                         kScrollbarW, rect_.h};
        const Rect thumb{track.x, rect_.y + tTop, kScrollbarW, tH};
        Color trackCol = style.border;
        trackCol.a *= 0.35f;
        Color thumbCol = style.traceBlue;
        thumbCol.a *= 0.85f;
        dl.fillRect(track, p.fill(trackCol), kScrollbarW * 0.5f);
        dl.fillRect(thumb, p.fill(thumbCol), kScrollbarW * 0.5f);
    }
    for (auto& c : children_) c->draw(dl, p);
}

// ------------------------------------------------------------ ScrollArea

void ScrollArea::layout(Rect r, const Style& style) {
    rect_ = r;
    windowedVisible_ = 0;
    if (children_.empty()) { contentH_ = 0.0f; return; }
    Rect inner = innerRect();
    Widget* content = children_.front().get();
    if (windowedMode() && content->dir == LayoutDir::Column) {
        // Windowed path: the content's children are uniform rows.
        // The content widget gets its full-height rect (rows are
        // positioned from it, scrolled), but its box layout does NOT
        // run: only the visible window of rows is laid out, and
        // off-window rows are set invisible so draw and hit testing
        // skip them too. Content padding is honored.
        const float rowH = windowRowHeight > 0.0f ? windowRowHeight : style.rowHeight;
        const auto& rows = content->children();
        const int n = static_cast<int>(rows.size());
        contentH_ = content->padding.t + content->padding.b +
                    static_cast<float>(n) * rowH;
        const float viewH = inner.h;
        scrollY_ = clampf(scrollY_, 0.0f, std::max(0.0f, contentH_ - viewH));
        Rect cr{inner.x + content->margin.l, inner.y + content->margin.t - scrollY_,
                (content->fixedW >= 0.0f) ? content->fixedW
                                          : std::max(0.0f, inner.w - content->margin.l -
                                                               content->margin.r),
                contentH_};
        content->setRect(cr);
        const float rowAreaX = cr.x + content->padding.l;
        const float rowAreaW = std::max(0.0f, cr.w - content->padding.l - content->padding.r);
        const float winTop = scrollY_ - static_cast<float>(kRowOverscan) * rowH;
        const float winBot = scrollY_ + viewH + static_cast<float>(kRowOverscan) * rowH;
        int visibleCount = 0;
        for (int i = 0; i < n; ++i) {
            Widget* row = rows[static_cast<std::size_t>(i)].get();
            const float rowTop = content->padding.t + static_cast<float>(i) * rowH;
            const bool inWindow = rowTop + rowH > winTop && rowTop < winBot;
            if (!inWindow) {
                row->setVisible(false);
                continue;
            }
            row->setVisible(true);
            ++visibleCount;
            const float w = (row->fixedW >= 0.0f)
                                ? row->fixedW
                                : std::max(0.0f, rowAreaW - row->margin.l - row->margin.r);
            const float h = (row->fixedH >= 0.0f)
                                ? row->fixedH
                                : std::max(0.0f, rowH - row->margin.t - row->margin.b);
            row->layout({rowAreaX + row->margin.l, cr.y + rowTop + row->margin.t, w, h},
                        style);
        }
        windowedVisible_ = visibleCount;
        return;
    }
    contentH_ = content->desiredHeight(style);
    float viewH = inner.h;
    scrollY_ = clampf(scrollY_, 0.0f, std::max(0.0f, contentH_ - viewH));
    Rect cr{inner.x + content->margin.l, inner.y + content->margin.t - scrollY_,
            (content->fixedW >= 0.0f) ? content->fixedW
                                      : std::max(0.0f, inner.w - content->margin.l - content->margin.r),
            contentH_};
    content->layout(cr, style);
}

bool ScrollArea::handlePointer(const PointerEvent& e) {
    if (e.type == PointerEvent::Type::Wheel) {
        scrollY_ -= e.wheelDelta * 24.0f;
        layout(rect_);
        return true;
    }
    return false;
}

void ScrollArea::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    drawBackgroundAndBorder(dl, p);
    dl.pushClip(rect_);
    for (auto& c : children_) c->draw(dl, p);
    dl.popClip();
}

// ----------------------------------------------------------- Collapsible

Collapsible::Collapsible(std::string id, std::string header, bool expanded)
    : Frame(std::move(id), Frame::InertFrameTag{}), header_(std::move(header)) {
    dir = LayoutDir::Column;
    writeState(WS_EXPANDED, expanded);
    // Alignment class default (build 19): an expandable header
    // is JD's second named exception to the center default - it
    // seats Left, its chevron leading. Instances may choose.
    setTextAlign(TextAlign::Left);
    // The body pad default (build 26, JD's rule): 10 design
    // units above and below the content inside the body frame.
    storeField(kBodyPadMask, kBodyPadShift,
               static_cast<std::uint64_t>(kDefaultBodyPad));
    // Composition (build 26): the inner Frame and its Column
    // box are created ONCE, here, and owned through the child
    // list like a Window's TitleBar; the Frame is owner-placed
    // (layoutExempt) and this widget's layout seats it. The box
    // fills the frame through its anchors; its margin - the
    // insets the content sits inside - is set at every layout
    // from the padding settings, so a pad change re-seats the
    // content on the next pass.
    auto body = std::make_unique<Frame>(id_ + "-body");
    body->setElastic(true);
    body->layoutExempt = true;
    body_ = body.get();
    addChild(std::move(body));
    auto box = std::make_unique<Panel>(id_ + "-body-box");
    box->dir = LayoutDir::Column;
    box->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
    box->setBackground(Color{0, 0, 0, 0});
    bodyBox_ = box.get();
    body_->addChild(std::move(box));
}

float Collapsible::contentDesiredHeight(const Style& style) const {
    float total = 0.0f;
    if (bodyBox_) {
        for (auto& c : bodyBox_->children())
            total += c->desiredHeight(style) + c->margin.t + c->margin.b;
    }
    for (auto& c : children_) {
        if (c.get() == body_) continue; // the frame is not content
        total += c->desiredHeight(style) + c->margin.t + c->margin.b;
    }
    return total;
}

float Collapsible::desiredHeight(const Style& style) const {
    // Build 26: the height derives from the title bar plus the
    // padded content (see the class comment); fixedH is not
    // consulted - a pinned height was exactly what reserved a
    // collapsed section's dead space and clipped an expanded
    // section's line in JD's Build 23 screenshots. Collapsed,
    // the height is the title bar alone.
    if (!expanded()) return headerHeight(style);
    return headerHeight(style) + 2.0f * bodyPad() +
           contentDesiredHeight(style);
}

void Collapsible::adoptContent() {
    if (!bodyBox_) return;
    for (std::size_t i = 0; i < children_.size();) {
        if (children_[i].get() == body_) { ++i; continue; }
        auto child = std::move(children_[i]);
        children_.erase(children_.begin() +
                        static_cast<std::ptrdiff_t>(i));
        bodyBox_->addChild(std::move(child));
    }
}

void Collapsible::layout(Rect r, const Style& style) {
    noteStyle(style);
    adoptContent();
    rect_ = r;
    headerH_ = headerHeight(style);
    if (!expanded()) {
        // The body frame is fully hidden by the title bar (JD's
        // rule): invisible, zero height at the title bar's foot,
        // and the content cascades to zero rects, so nothing of
        // it is drawn, hit, or measured into any extent.
        body_->setVisible(false);
        body_->setRect({r.x, r.y + headerH_, r.w, 0.0f});
        bodyBox_->setRect({r.x, r.y, 0, 0});
        for (auto& c : bodyBox_->children())
            c->layout({r.x, r.y, 0, 0}, style);
        return;
    }
    body_->setVisible(true);
    // The body box's margin IS the pad the content sits inside:
    // the widget's own padding on the sides, bodyPad above and
    // below (JD's default 10). Refreshed every layout so a
    // settings write re-seats the content on the next pass.
    bodyBox_->margin.l = padding.l;
    bodyBox_->margin.t = bodyPad();
    bodyBox_->margin.r = padding.r;
    bodyBox_->margin.b = bodyPad();
    const float bodyH = std::max(0.0f, r.h - headerH_);
    body_->layout({r.x, r.y + headerH_, r.w, bodyH}, style);
}

bool Collapsible::handlePointer(const PointerEvent& e) {
    Rect header{rect_.x, rect_.y, rect_.w, headerH_};
    if (e.button != 0 || !header.contains(e.x, e.y)) return false;
    if (e.type == PointerEvent::Type::Up) {
        flipState(WS_EXPANDED);
        if (onToggle) onToggle(*this, expanded());
        return true;
    }
    return e.type == PointerEvent::Type::Down;
}

void Collapsible::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const_cast<Collapsible*>(this)->headerH_ = style.rowHeight + 4.0f;
    drawBackgroundAndBorder(dl, p);
    Rect header{rect_.x, rect_.y, rect_.w, headerH_};
    dl.fillRect(header, p.fill(style.titleBar), style.cornerRadius);
    TextStyle ts = textStyle ? *textStyle : style.titleText;
    if (ts.color.a <= 0.0f) ts.color = style.text;
    float tw = metrics.measure(header_, ts);
    // Build 19: the (chevron + gap + header text) unit seats in
    // the padded box, anchored per the alignment setting (Left
    // by this class's default). The unit's internal geometry is
    // the historical one: chevron ink starts at the unit's left
    // edge, text 13 past it - at the default pad the unit lands
    // exactly where it always has (chevron center rect_.x + 9,
    // text rect_.x + 18).
    const float pad = textPad();
    const float unitW = 13.0f + tw;
    const float unitX = rect_.x + pad + alignOffset(unitW, rect_.w - 2.0f * pad);
    float cx = unitX + 4.0f;
    float cy = rect_.y + headerH_ * 0.5f;
    if (expanded()) {
        dl.line({cx - 4.0f, cy - 2.0f}, {cx + 4.0f, cy - 2.0f}, style.textMuted);
        dl.line({cx + 4.0f, cy - 2.0f}, {cx, cy + 4.0f}, style.textMuted);
        dl.line({cx, cy + 4.0f}, {cx - 4.0f, cy - 2.0f}, style.textMuted);
    } else {
        dl.line({cx - 2.0f, cy - 4.0f}, {cx - 2.0f, cy + 4.0f}, style.textMuted);
        dl.line({cx - 2.0f, cy + 4.0f}, {cx + 4.0f, cy}, style.textMuted);
        dl.line({cx + 4.0f, cy}, {cx - 2.0f, cy - 4.0f}, style.textMuted);
    }
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    dl.textRun({unitX + 13.0f, rect_.y + (headerH_ - ts.size) * 0.5f, tw, ts.size},
               header_, ts, p.fill(ts.color), fm.ascent);
    if (expanded()) {
        for (auto& c : children_) c->draw(dl, p);
    }
}

// ------------------------------------------------------------- TextField

void TextField::setText(const std::string& t) {
    text_ = t;
    cursor_ = text_.size();
    selAnchor_ = cursor_;
    dispStart_ = 0;
    clampScrollState();
}

void TextField::clampScrollState() {
    auto snap = [this](std::size_t pos) {
        pos = std::min(pos, text_.size());
        if (pos > 0 && pos < text_.size() &&
            (static_cast<unsigned char>(text_[pos]) & 0xC0) == 0x80)
            pos = prevCpBoundary(text_, pos);
        return pos;
    };
    cursor_ = snap(cursor_);
    selAnchor_ = snap(selAnchor_);
    dispStart_ = snap(dispStart_);
    int lineCount = 1;
    for (char c : text_) if (c == '\n') ++lineCount;
    scrollLine_ = std::max(0, std::min(scrollLine_, lineCount - 1));
}

float TextField::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    return style.rowHeight + 8.0f;
}

void TextField::deleteSelection() {
    if (!hasSelection()) return;
    std::size_t s = selectionStart();
    text_.erase(s, selectionEnd() - s);
    cursor_ = selAnchor_ = s;
    clampScrollState();
}

std::string TextField::selectedText() const {
    if (!hasSelection()) return "";
    return text_.substr(selectionStart(), selectionEnd() - selectionStart());
}

void TextField::selectAll() {
    selAnchor_ = 0;
    cursor_ = text_.size();
}

void TextField::insertText(const std::string& utf8) {
    deleteSelection();
    std::string ins;
    ins.reserve(utf8.size());
    for (char c : utf8) {
        if (!multiLine() && (c == '\n' || c == '\r')) continue;
        ins += c;
    }
    if (!ins.empty()) {
        text_.insert(cursor_, ins);
        cursor_ += ins.size();
    }
    selAnchor_ = cursor_;
    // The inserted bytes may end mid-sequence against the following
    // buffer byte; re-snap caret/anchor and clamp the display state
    // against the new buffer (review CORE-H6 + CORE-M10).
    clampScrollState();
}

// ------------------------------------------------------------- IME
void TextField::imeBegin() {
    preedit_.clear();
    raiseState(WS_COMPOSING);
}

void TextField::imeUpdate(const std::string& preedit) {
    // Lenient: an update without an explicit begin starts the
    // composition (some hosts only ever send updates + commit).
    raiseState(WS_COMPOSING);
    preedit_ = preedit;
}

void TextField::imeCommit(const std::string& finalText) {
    std::string ins = finalText.empty() ? preedit_ : finalText;
    preedit_.clear();
    dropState(WS_COMPOSING);
    if (!ins.empty()) insertText(ins);
}

void TextField::imeCancel() {
    preedit_.clear();
    dropState(WS_COMPOSING);
    clampScrollState();
}

// ---------------------------------------------------- logical lines
std::size_t TextField::lineStartOffset(std::size_t bytePos) const {
    std::size_t p = std::min(bytePos, text_.size());
    while (p > 0 && text_[p - 1] != '\n') --p;
    return p;
}

std::size_t TextField::lineEndOffset(std::size_t bytePos) const {
    std::size_t p = std::min(bytePos, text_.size());
    while (p < text_.size() && text_[p] != '\n') ++p;
    return p;
}

void TextField::moveVertical(int dy, bool extend) {
    const std::size_t ls = lineStartOffset(cursor_);
    std::size_t col = 0; // caret column, in code points
    for (std::size_t i = ls; i < cursor_; i = nextCpBoundary(text_, i)) ++col;
    std::size_t target = cursor_;
    if (dy < 0) {
        if (ls == 0) return; // already on the first line: stay
        const std::size_t pls = lineStartOffset(ls - 1);
        const std::size_t ple = ls - 1; // previous line's end (its '\n')
        target = pls;
        for (std::size_t c = 0; c < col && target < ple; ++c)
            target = nextCpBoundary(text_, target);
    } else {
        const std::size_t le = lineEndOffset(cursor_);
        if (le >= text_.size()) return; // already on the last line
        const std::size_t nls = le + 1;
        const std::size_t nle = lineEndOffset(nls);
        target = nls;
        for (std::size_t c = 0; c < col && target < nle; ++c)
            target = nextCpBoundary(text_, target);
    }
    cursor_ = target;
    if (!extend) selAnchor_ = cursor_;
}

void TextField::singleLineWindow(const std::string& disp, std::size_t dispCursor,
                                 const TextStyle& ts, const ITextMetrics& m,
                                 std::size_t& outStart, std::size_t& outEnd,
                                 float& outOriginX) const {
    // Display window (code point boundaries only), so the caret
    // stays visible while typing - the window draw has always
    // used, now computed against the padded box (build 19) and
    // shared with the pointer mapping.
    const float maxW = rect_.w - 2.0f * textPad();
    std::size_t start = 0;
    std::size_t end = disp.size();
    if (maxW > 0.0f && m.measure(disp, ts) > maxW) {
        std::size_t s = dispCursor;
        while (s > 0) {
            std::size_t prev = prevCpBoundary(disp, s);
            if (m.measure(disp.substr(prev, dispCursor - prev), ts) > maxW) break;
            s = prev;
        }
        start = s;
        std::size_t en = dispCursor;
        while (en < disp.size()) {
            std::size_t nx = nextCpBoundary(disp, en);
            if (m.measure(disp.substr(start, nx - start), ts) > maxW) break;
            en = nx;
        }
        end = en;
    }
    outStart = start;
    outEnd = end;
    const float sliceW =
        end > start ? m.measure(disp.substr(start, end - start), ts) : 0.0f;
    outOriginX = rect_.x + textPad() +
                 alignOffset(sliceW, std::max(0.0f, maxW));
}

float TextField::lineOriginX(const std::string& buf, std::size_t ls, std::size_t le,
                             const TextStyle& ts, const ITextMetrics& m) const {
    const float w = le > ls ? m.measure(buf.substr(ls, le - ls), ts) : 0.0f;
    return rect_.x + textPad() +
           alignOffset(w, std::max(0.0f, rect_.w - 2.0f * textPad()));
}

std::size_t TextField::offsetFromPoint(float x, float y) const {
    return offsetFromPointBuf(text_, x, y, cursor_);
}

// The mapping walks, shared by the real buffer and (during IME
// composition) the display buffer. Style comes from the last layout
// (review CORE-L6), letterSpacing is charged only BETWEEN code
// points (matching ITextMetrics::measure), and every starting
// offset is clamped against the buffer first (review CORE-H6).
std::size_t TextField::offsetFromPointBuf(const std::string& buf, float x, float y,
                                          std::size_t dispCursor) const {
    if (!multiLine()) return offsetFromXBuf(buf, x, dispCursor);
    static const ApproxTextMetrics approx;
    TextStyle ts = effectiveTextStyle(lastStyle());
    const float lineH = approx.lineHeight(ts);
    // Line starts over the whole buffer (byte offsets).
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < buf.size(); ++i)
        if (buf[i] == '\n') starts.push_back(i + 1);
    int li = std::max(0, scrollLine_);
    if (lineH > 0.0f) li += static_cast<int>((y - rect_.y - textPad()) / lineH);
    li = std::max(0, std::min(li, static_cast<int>(starts.size()) - 1));
    const std::size_t ls = starts[static_cast<std::size_t>(li)];
    std::size_t le = ls;
    while (le < buf.size() && buf[le] != '\n') ++le;
    // The target measures from the line's own origin - the same
    // padded, alignment-anchored origin draw used (build 19).
    const float target = x - lineOriginX(buf, ls, le, ts, approx);
    if (target <= 0.0f) return ls;
    float acc = 0.0f;
    std::size_t i = ls;
    while (i < le) {
        std::size_t cs = i;
        std::size_t nx = nextCpBoundary(buf, cs);
        std::size_t save = cs;
        char32_t c = nextCodePoint(buf, save);
        float adv = approx.glyphAdvance(c, ts.font, ts.size, ts.weight);
        if (nx < le) adv += ts.letterSpacing; // spacing lives between cps
        if (acc + adv * 0.5f >= target) return cs;
        acc += adv;
        i = nx;
    }
    return le;
}

void TextField::selectWordAt(std::size_t bytePos) {
    if (text_.empty()) return;
    // Classify the code point at the press (the one before the caret
    // position when the press lands at the very end).
    std::size_t probe = bytePos;
    if (probe >= text_.size()) probe = prevCpBoundary(text_, text_.size());
    std::size_t save = probe;
    char32_t cp = nextCodePoint(text_, save);
    auto cls = [](char32_t c) {
        if (c == U' ' || c == U'\t' || c == U'\n') return 1; // whitespace run
        if (c >= 0x80 || (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') ||
            (c >= U'a' && c <= U'z') || c == U'_')
            return 0; // word run (letters, digits, '_', any non-ASCII)
        return 2;     // punctuation run
    };
    const int want = cls(cp);
    std::size_t lo = probe;
    while (lo > 0) {
        std::size_t p = prevCpBoundary(text_, lo);
        std::size_t s2 = p;
        if (cls(nextCodePoint(text_, s2)) != want) break;
        lo = p;
    }
    std::size_t hi = probe;
    while (hi < text_.size()) {
        std::size_t s2 = hi;
        if (cls(nextCodePoint(text_, s2)) != want) break;
        hi = nextCpBoundary(text_, hi);
    }
    selAnchor_ = lo;
    cursor_ = hi;
}

std::size_t TextField::offsetFromX(float x) const {
    return offsetFromXBuf(text_, x, cursor_);
}

std::size_t TextField::offsetFromXBuf(const std::string& buf, float x,
                                      std::size_t dispCursor) const {
    // Pointer mapping uses the approximate provider, like the other
    // layout-time hit decisions in the base (Tabs strip); the shipping
    // provider refines draw, the caret lands on the same code point
    // under any provider whose advances are per-code-point sums.
    static const ApproxTextMetrics approx;
    TextStyle ts = effectiveTextStyle(lastStyle());
    // The window and origin are recomputed from the same inputs
    // draw used (build 19: singleLineWindow), so the walk starts
    // at the visible slice and measures from its drawn origin -
    // pad and alignment included - instead of a baked-in inset.
    std::size_t start = 0, end = 0;
    float originX = rect_.x + textPad();
    singleLineWindow(buf, std::min(dispCursor, buf.size()), ts, approx,
                     start, end, originX);
    float target = x - originX;
    std::size_t i = start;
    if (target <= 0.0f) return i;
    float acc = 0.0f;
    while (i < end) {
        std::size_t cs = i;
        std::size_t nx = nextCpBoundary(buf, cs);
        std::size_t save = cs;
        char32_t c = nextCodePoint(buf, save);
        float adv = approx.glyphAdvance(c, ts.font, ts.size, ts.weight);
        if (nx < buf.size()) adv += ts.letterSpacing;
        if (acc + adv * 0.5f >= target) return cs;
        acc += adv;
        i = nx;
    }
    return end;
}

std::size_t TextField::mapPointer(float x, float y) const {
    if (!composing() || preedit_.empty()) return offsetFromPoint(x, y);
    // Map over the display buffer (buffer + preedit spliced at the
    // caret, exactly as draw renders it), then convert back: offsets
    // inside the preedit land on the composition anchor.
    std::string disp = text_;
    disp.insert(cursor_, preedit_);
    std::size_t d = offsetFromPointBuf(disp, x, y, cursor_ + preedit_.size());
    if (d <= cursor_) return d;
    if (d >= cursor_ + preedit_.size()) return d - preedit_.size();
    return cursor_;
}

bool TextField::handlePointer(const PointerEvent& e) {
    if (e.button != 0) return false;
    switch (e.type) {
        case PointerEvent::Type::Down: {
            std::size_t pos = mapPointer(e.x, e.y);
            if (e.clickCount >= 2) {
                // Double click: select the word at the press and do
                // NOT arm a drag selection on top of it.
                selectWordAt(pos);
                return true;
            }
            cursor_ = selAnchor_ = pos;
            raiseState(WS_PRESSED);
            return true;
        }
        case PointerEvent::Type::Move:
            if (hasState(WS_PRESSED)) { // drag selection: anchor stays
                cursor_ = mapPointer(e.x, e.y);
                return true;
            }
            return false;
        case PointerEvent::Type::Up:
            if (hasState(WS_PRESSED)) { dropState(WS_PRESSED); return true; }
            return false;
        case PointerEvent::Type::Wheel:
            return false;
    }
    return false;
}

bool TextField::handleKey(const KeyEvent& e) {
    if (!e.down) return false;
    // Clipboard / select-all chords: intent triggers only. The shell
    // owns the clipboard; copy/cut hand text out through the
    // callbacks, paste asks and the shell answers with insertText().
    if (e.ctrl) {
        switch (e.ch) {
            case U'c': case U'C':
                // Copy is read-only: it stays live during IME
                // composition (it cannot mutate the buffer).
                if (hasSelection() && onCopy) onCopy(*this, selectedText());
                return true;
            case U'x': case U'X':
                // Cut, paste and select-all are gated while an IME
                // composition is live (round-2 review C11): cut and
                // select-all would mutate the buffer / selection
                // under the composition anchor, and a paste answer
                // lands in insertText, the same mutation path. The
                // chords are consumed silently, like the other
                // composition-gated keys below.
                if (composing()) return true;
                if (hasSelection()) {
                    if (onCut) onCut(*this, selectedText());
                    deleteSelection();
                }
                return true;
            case U'v': case U'V':
                if (composing()) return true;
                if (onPasteRequest) onPasteRequest(*this);
                return true;
            case U'a': case U'A':
                if (composing()) return true;
                selectAll();
                return true;
            default:
                return false; // other chords stay the shell's business
        }
    }
    // While an IME composition is live, buffer-mutating keys belong
    // to the shell's IME, not to the buffer (see the class contract):
    // they are consumed here without touching text_.
    const bool composing_ = composing();
    switch (e.key) {
        case KEY_BACKSPACE:
            if (composing_) return true;
            if (hasSelection()) deleteSelection();
            else if (cursor_ > 0) {
                std::size_t prev = prevCpBoundary(text_, cursor_);
                text_.erase(prev, cursor_ - prev);
                cursor_ = selAnchor_ = prev;
                clampScrollState();
            }
            return true;
        case KEY_DELETE:
            if (composing_) return true;
            if (hasSelection()) deleteSelection();
            else if (cursor_ < text_.size()) {
                std::size_t nx = nextCpBoundary(text_, cursor_);
                text_.erase(cursor_, nx - cursor_);
                selAnchor_ = cursor_;
                clampScrollState();
            }
            return true;
        case KEY_LEFT:
            if (e.shift) {
                if (cursor_ > 0) cursor_ = prevCpBoundary(text_, cursor_);
            } else if (hasSelection()) {
                cursor_ = selAnchor_ = selectionStart();
            } else if (cursor_ > 0) {
                cursor_ = selAnchor_ = prevCpBoundary(text_, cursor_);
            }
            return true;
        case KEY_RIGHT:
            if (e.shift) {
                if (cursor_ < text_.size()) cursor_ = nextCpBoundary(text_, cursor_);
            } else if (hasSelection()) {
                cursor_ = selAnchor_ = selectionEnd();
            } else if (cursor_ < text_.size()) {
                cursor_ = selAnchor_ = nextCpBoundary(text_, cursor_);
            }
            return true;
        case KEY_HOME:
            cursor_ = lineStartOffset(cursor_);
            if (!e.shift) selAnchor_ = cursor_;
            return true;
        case KEY_END:
            cursor_ = lineEndOffset(cursor_);
            if (!e.shift) selAnchor_ = cursor_;
            return true;
        case KEY_UP:
            if (multiLine()) { moveVertical(-1, e.shift); return true; }
            return false; // single-line: bubbles (e.g. to a ComboBox)
        case KEY_DOWN:
            if (multiLine()) { moveVertical(+1, e.shift); return true; }
            return false;
        case KEY_ENTER:
            if (composing_) return true;
            if (multiLine()) { insertText("\n"); return true; }
            if (onSubmit) onSubmit(*this, text_);
            return true;
        default: break;
    }
    // Committed character input: any printable code point (shells send
    // IME-committed text the same way, one event per code point).
    // During composition the shell's IME owns character keys too.
    if (composing_) return e.ch >= 0x20 && e.ch != 0x7f;
    if (e.ch >= 0x20 && e.ch != 0x7f) {
        std::string enc;
        utf8Encode(e.ch, enc);
        insertText(enc);
        return true;
    }
    return false;
}

void TextField::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    noteStyle(style);
    const ITextMetrics& metrics = *p.metrics;
    dl.fillRect(rect_, p.fill(style.controlBg), style.cornerRadius);
    dl.polyline(rectOutline(rect_), (this == p.focused) ? style.tracePurple : style.border,
                style.borderWidth);
    TextStyle ts = effectiveTextStyle(style);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);

    // Composition display: while an IME composition is live, the
    // preedit string is spliced into a display copy at the caret; the
    // buffer itself is untouched until imeCommit. The preedit segment
    // is emitted as its own run (trace purple + underline) and the
    // selection highlight is suppressed for the duration.
    std::string disp = text_;
    const bool comp = composing() && !preedit_.empty();
    std::size_t preStart = cursor_, preEnd = cursor_;
    std::size_t dispCursor = cursor_;
    if (comp) {
        disp.insert(cursor_, preedit_);
        preEnd = cursor_ + preedit_.size();
        dispCursor = preEnd;
    }
    if (multiLine()) {
        const float lineH = metrics.lineHeight(ts);
        // Logical lines of the display text: (start, end) byte ranges
        // excluding the '\n'.
        std::vector<std::pair<std::size_t, std::size_t>> lines;
        for (std::size_t s = 0;;) {
            std::size_t e = s;
            while (e < disp.size() && disp[e] != '\n') ++e;
            lines.emplace_back(s, e);
            if (e >= disp.size()) break;
            s = e + 1;
        }
        int caretLine = 0;
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (dispCursor <= lines[i].second) { caretLine = static_cast<int>(i); break; }
        const int visLines =
            std::max(1, static_cast<int>((rect_.h - 2.0f * textPad()) / lineH));
        if (caretLine < scrollLine_) scrollLine_ = caretLine;
        if (caretLine >= scrollLine_ + visLines)
            scrollLine_ = caretLine - visLines + 1;
        scrollLine_ = std::max(0, std::min(scrollLine_,
                                           static_cast<int>(lines.size()) - 1));
        dl.pushClip(rect_);
        const int last = std::min(static_cast<int>(lines.size()),
                                  scrollLine_ + visLines);
        for (int li = scrollLine_; li < last; ++li) {
            const auto [ls, le] = lines[static_cast<std::size_t>(li)];
            const float lineY = rect_.y + textPad() + (li - scrollLine_) * lineH;
            const float textY = lineY + (lineH - ts.size) * 0.5f;
            // The line's own origin: padded, alignment-anchored
            // (build 19), shared with the pointer mapping.
            const float lineX = lineOriginX(disp, ls, le, ts, metrics);
            if (!comp && hasSelection() && this == p.focused) {
                std::size_t ss = std::max(selectionStart(), ls);
                std::size_t se = std::min(selectionEnd(), le);
                if (se > ss) {
                    float sx1 = lineX + metrics.measure(disp.substr(ls, ss - ls), ts);
                    float sx2 = lineX + metrics.measure(disp.substr(ls, se - ls), ts);
                    dl.fillRect({sx1, lineY + 1.0f, sx2 - sx1, lineH - 2.0f},
                                p.fill(style.selection), 0.0f);
                }
            }
            // Emit the line, splitting around the preedit segment
            // when this is the caret's line during a composition.
            auto emitRun = [&](std::size_t a, std::size_t b, Color col, float x) {
                if (b <= a) return x;
                std::string seg = disp.substr(a, b - a);
                float w = metrics.measure(seg, ts);
                dl.textRun({x, textY, w, ts.size}, seg, ts, p.fill(col), fm.ascent);
                return x + w;
            };
            float x = lineX;
            if (comp && li == caretLine) {
                x = emitRun(ls, std::max(ls, std::min(preStart, le)), ts.color, x);
                float preX = x;
                x = emitRun(std::max(ls, preStart), std::min(preEnd, le),
                            style.tracePurple, x);
                dl.line({preX, textY + ts.size + 2.0f}, {x, textY + ts.size + 2.0f},
                        style.tracePurple, 1.0f);
                x = emitRun(std::max(ls, preEnd), le, ts.color, x);
            } else {
                emitRun(ls, le, ts.color, x);
            }
            if (this == p.focused && li == caretLine) {
                float cx = lineX + metrics.measure(
                    disp.substr(ls, std::min(dispCursor, le) - ls), ts);
                dl.line({cx, lineY + 2.0f}, {cx, lineY + lineH - 2.0f},
                        style.traceBlue, 1.0f);
            }
        }
        dl.popClip();
        for (auto& c : children_) c->draw(dl, p);
        return;
    }

    // Single-line: the display window and its origin come from
    // singleLineWindow (build 19) - the padded box bounds the
    // window, and the slice anchors per the alignment setting.
    std::size_t start = 0;
    std::size_t end = 0;
    float x0 = rect_.x + textPad();
    singleLineWindow(disp, dispCursor, ts, metrics, start, end, x0);
    dispStart_ = start;
    float textY = rect_.y + (rect_.h - ts.size) * 0.5f;
    if (!comp && hasSelection() && this == p.focused) {
        std::size_t ss = std::max(selectionStart(), start);
        std::size_t se = std::min(selectionEnd(), end);
        if (se > ss) {
            float sx1 = x0 + metrics.measure(disp.substr(start, ss - start), ts);
            float sx2 = x0 + metrics.measure(disp.substr(start, se - start), ts);
            dl.fillRect({sx1, rect_.y + textPad(), sx2 - sx1,
                         rect_.h - 2.0f * textPad()},
                        p.fill(style.selection), 0.0f);
        }
    }
    if (comp) {
        auto emitRun = [&](std::size_t a, std::size_t b, Color col, float x) {
            if (b <= a) return x;
            std::string seg = disp.substr(a, b - a);
            float w = metrics.measure(seg, ts);
            dl.textRun({x, textY, w, ts.size}, seg, ts, p.fill(col), fm.ascent);
            return x + w;
        };
        float x = x0;
        x = emitRun(start, std::max(start, std::min(preStart, end)), ts.color, x);
        float preX = x;
        x = emitRun(std::max(start, preStart), std::min(preEnd, end),
                    style.tracePurple, x);
        dl.line({preX, rect_.y + rect_.h - textPad()},
                {x, rect_.y + rect_.h - textPad()}, style.tracePurple, 1.0f);
        emitRun(std::max(start, preEnd), end, ts.color, x);
    } else {
        std::string shown = disp.substr(start, end - start);
        float tw = metrics.measure(shown, ts);
        dl.textRun({x0, textY, tw, ts.size}, shown, ts, p.fill(ts.color), fm.ascent);
    }
    if (this == p.focused) {
        float cx = x0 + metrics.measure(disp.substr(start, dispCursor - start), ts);
        dl.line({cx, rect_.y + textPad()}, {cx, rect_.y + rect_.h - textPad()},
                style.traceBlue, 1.0f);
    }
    for (auto& c : children_) c->draw(dl, p);
}

// ---------------------------------------------------------------- Slider

Slider::Slider(std::string id, float minValue, float maxValue, float value, float step)
    : Frame(std::move(id), Frame::InertFrameTag{}), min_(minValue), max_(maxValue), step_(step) {
    if (max_ < min_) std::swap(min_, max_);
    value_.store(clampSnap(value), std::memory_order_relaxed);
}

float Slider::clampSnap(float v) const {
    v = clampf(v, min_, max_);
    if (step_ > 0.0f) {
        float n = std::round((v - min_) / step_);
        v = clampf(min_ + n * step_, min_, max_);
    }
    return v;
}

void Slider::setValue(float v) {
    value_.store(clampSnap(v), std::memory_order_relaxed);
}

void Slider::applyInput(float v) {
    float nv = clampSnap(v);
    float old = value_.load(std::memory_order_relaxed);
    if (nv != old) {
        value_.store(nv, std::memory_order_relaxed);
        if (onChange) onChange(*this, nv);
    }
}

float Slider::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    return style.rowHeight;
}

// Track geometry: the thumb is 14 px wide and travels the inner
// span - the padded box's span (build 19: the slider is a purely
// graphical control, and the pad seats its track and thumb like
// any other content). Draw uses the identical span, so the value
// a click computes is the value the thumb shows.
float Slider::valueFromX(float x) const {
    const float thumbW = 14.0f;
    float span = std::max(1.0f, rect_.w - 2.0f * textPad() - thumbW);
    float t = clampf((x - rect_.x - textPad() - thumbW * 0.5f) / span, 0.0f, 1.0f);
    return min_ + t * (max_ - min_);
}

bool Slider::handlePointer(const PointerEvent& e) {
    if (e.button != 0) return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            if (!rect_.contains(e.x, e.y)) return false;
            raiseState(WS_DRAGGING);
            applyInput(valueFromX(e.x));
            return true;
        case PointerEvent::Type::Move:
            if (hasState(WS_DRAGGING)) { applyInput(valueFromX(e.x)); return true; }
            return false;
        case PointerEvent::Type::Up:
            if (hasState(WS_DRAGGING)) {
                dropState(WS_DRAGGING);
                if (onCommit) onCommit(*this, value());
                return true;
            }
            return false;
        case PointerEvent::Type::Wheel:
            return false;
    }
    return false;
}

bool Slider::handleKey(const KeyEvent& e) {
    if (!e.down) return false;
    float delta = (step_ > 0.0f) ? step_ : (max_ - min_) / 20.0f;
    if (e.key == KEY_LEFT || e.key == KEY_DOWN) { applyInput(value() - delta); return true; }
    if (e.key == KEY_RIGHT || e.key == KEY_UP) { applyInput(value() + delta); return true; }
    if (e.key == KEY_HOME) { applyInput(min_); return true; }
    if (e.key == KEY_END) { applyInput(max_); return true; }
    return false;
}

void Slider::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    drawBackgroundAndBorder(dl, p);
    const float thumbW = 14.0f, thumbH = 18.0f, trackH = 6.0f;
    float range = max_ - min_;
    float t = range > 0.0f ? (value() - min_) / range : 0.0f;
    Rect track{rect_.x + textPad() + thumbW * 0.5f,
               rect_.y + (rect_.h - trackH) * 0.5f,
               std::max(0.0f, rect_.w - 2.0f * textPad() - thumbW), trackH};
    dl.fillRect(track, p.fill(style.controlBg), trackH * 0.5f);
    Rect filled = track;
    filled.w = track.w * t;
    dl.fillRect(filled, style.traceBlue, trackH * 0.5f);
    float thumbCx = track.x + track.w * t;
    Rect thumb{thumbCx - thumbW * 0.5f, rect_.y + (rect_.h - thumbH) * 0.5f, thumbW, thumbH};
    dl.fillRect(thumb, style.text, 3.0f);
    if (this == p.focused || hasState(WS_DRAGGING))
        dl.polyline(rectOutline(rect_), style.tracePurple, style.borderWidth);
    for (auto& c : children_) c->draw(dl, p);
}

// ------------------------------------------------------------------ Tabs

Widget* Tabs::addTab(const std::string& label, std::unique_ptr<Widget> page) {
    // The active tab is one bit of a 64-bit quad-word mask: past 64
    // tabs there is no bit to activate with (the cap was 32 with the
    // old DWORD mask, review CORE-L1; widened in the state-packing
    // pass). Refuse and do not adopt the page.
    if (labels_.size() >= kMaxTabs) return nullptr;
    labels_.push_back(label);
    Widget* p = addChild(std::move(page));
    if (!p) { labels_.pop_back(); return nullptr; }
    if (labels_.size() == 1) activeMask_.store(1ull, std::memory_order_relaxed);
    return p;
}

int Tabs::activeIndex() const {
    std::uint64_t m = activeMask_.load(std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i) if (m & (1ull << i)) return i;
    return -1;
}

void Tabs::setActiveIndex(int i) {
    if (i < 0 || i >= static_cast<int>(labels_.size())) return;
    if (i >= 64) return; // unreachable while kMaxTabs == 64; keeps the shift honest
    activeMask_.store(1ull << i, std::memory_order_relaxed);
}

void Tabs::activate(int i) {
    if (i == activeIndex()) return;
    setActiveIndex(i);
    if (onSelect) onSelect(*this, i);
}

std::vector<float> Tabs::tabWidths(const Style& style, const ITextMetrics& m) const {
    std::vector<float> widths;
    TextStyle ts = textStyle ? *textStyle : style.bodyText;
    for (auto& label : labels_) widths.push_back(m.measure(label, ts) + 24.0f);
    return widths;
}

float Tabs::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    float page = 0.0f;
    int active = activeIndex();
    if (active >= 0 && active < static_cast<int>(children_.size()))
        page = children_[static_cast<std::size_t>(active)]->desiredHeight(style);
    return stripHeight(style) + page + padding.t + padding.b;
}

void Tabs::layout(Rect r, const Style& style) {
    noteStyle(style);
    rect_ = r;
    lastLayoutW_ = r.w;
    float strip = stripHeight(style);
    Rect pageArea{r.x + padding.l, r.y + strip + padding.t,
                  std::max(0.0f, r.w - padding.l - padding.r),
                  std::max(0.0f, r.h - strip - padding.t - padding.b)};
    int active = activeIndex();
    for (std::size_t i = 0; i < children_.size(); ++i) {
        if (static_cast<int>(i) == active) children_[i]->layout(pageArea, style);
        else children_[i]->layout({pageArea.x, pageArea.y, 0, 0}, style);
    }
}

bool Tabs::handlePointer(const PointerEvent& e) {
    // Strip geometry from the last-used Style (review CORE-L2), the
    // same one draw used, never defaultStyle().
    if (e.button != 0 || e.type != PointerEvent::Type::Up) {
        return e.button == 0 && e.type == PointerEvent::Type::Down &&
               e.y >= rect_.y && e.y < rect_.y + stripHeight(lastStyle());
    }
    float strip = stripHeight(lastStyle());
    if (e.y < rect_.y || e.y >= rect_.y + strip) return false;
    auto widths = tabWidths(lastStyle(), approx_);
    float x = rect_.x + textPad(); // segments start at the padded box (as drawn)
    for (std::size_t i = 0; i < widths.size(); ++i) {
        if (e.x >= x && e.x < x + widths[i]) { activate(static_cast<int>(i)); return true; }
        x += widths[i];
    }
    return true; // clicks on the strip itself are consumed
}

bool Tabs::handleKey(const KeyEvent& e) {
    if (!e.down || labels_.empty()) return false;
    int n = static_cast<int>(labels_.size());
    if (e.key == KEY_LEFT) { activate((activeIndex() + n - 1) % n); return true; }
    if (e.key == KEY_RIGHT) { activate((activeIndex() + 1) % n); return true; }
    return false;
}

void Tabs::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    noteStyle(style);
    const ITextMetrics& metrics = *p.metrics;
    drawBackgroundAndBorder(dl, p);
    float strip = stripHeight(style);
    Rect stripRect{rect_.x, rect_.y, rect_.w, strip};
    dl.fillRect(stripRect, p.fill(style.titleBar), style.cornerRadius);
    auto widths = tabWidths(style, metrics);
    TextStyle ts = textStyle ? *textStyle : style.bodyText;
    if (ts.color.a <= 0.0f) ts.color = style.text;
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    int active = activeIndex();
    // Segments tile from the padded box's left edge (build 19);
    // each label anchors within its segment per the alignment
    // setting (Center by default - the historical centered tab
    // label - with the segment's own 12-unit side insets as the
    // anchor's stops for Left / Right).
    float x = rect_.x + textPad();
    for (std::size_t i = 0; i < labels_.size(); ++i) {
        bool isActive = static_cast<int>(i) == active;
        Rect tab{x, rect_.y, widths[i], strip};
        if (isActive) dl.fillRect(tab, p.fill(style.controlBgHot), style.cornerRadius);
        float tw = metrics.measure(labels_[i], ts);
        float lx = x + (widths[i] - tw) * 0.5f;
        if (textAlign() == TextAlign::Left) lx = x + 12.0f;
        else if (textAlign() == TextAlign::Right) lx = x + widths[i] - 12.0f - tw;
        dl.textRun({lx, rect_.y + (strip - ts.size) * 0.5f, tw, ts.size},
                   labels_[i], ts, p.fill(isActive ? ts.color : style.textMuted), fm.ascent);
        if (isActive) {
            dl.line({tab.x + 2.0f, tab.y + tab.h - 2.0f}, {tab.x + tab.w - 2.0f, tab.y + tab.h - 2.0f},
                    style.traceBlue, 2.0f);
        }
        x += widths[i];
    }
    if (active >= 0 && active < static_cast<int>(children_.size()))
        children_[static_cast<std::size_t>(active)]->draw(dl, p);
}

// -------------------------------------------------------------- ComboBox

void ComboBox::attach(UiContext& ctx) {
    if (ctx_ == &ctx) return;
    if (ctx_) ctx_->unregisterContextClient(this);
    ctx_ = &ctx;
    ctx_->registerContextClient(this);
}

ComboBox::~ComboBox() {
    // Unregister so a context that outlives the box never calls
    // detachContext on a dead widget (review C3's other half).
    if (ctx_) ctx_->unregisterContextClient(this);
}

std::string ComboBox::selectedText() const {
    if (selected_ >= 0 && selected_ < static_cast<int>(items_.size()))
        return items_[static_cast<std::size_t>(selected_)];
    return "";
}

void ComboBox::setSelectedIndex(int i) {
    if (i >= -1 && i < static_cast<int>(items_.size())) selected_ = i;
}

float ComboBox::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    return style.rowHeight + 8.0f;
}

void ComboBox::openMenu(UiContext& ctx) {
    attach(ctx); // registers for context-death detachment (review C3)
    if (!menu_) {
        menu_ = std::make_unique<Popup>(id_ + "-menu", Popup::Flavor::Menu);
        menu_->padding = Edges(0.0f);
        // The composed panel (build 23, JD's dropdown ruling;
        // docs/COMPOSITION.md): the Popup holds a Frame, the
        // Frame holds the ListBox. The Frame is elastic - its
        // design space IS the panel the Popup hands it, so the
        // panel's geometry is the menu frame's geometry at any
        // item count - and it carries the panel's face: the
        // theme's panel fill (refreshed from the context's
        // style at every open, below) and the Trace border.
        // Before this build the Popup and its list both drew
        // with the Widget base's transparent default, so the
        // option text floated bare over whatever the scene had
        // painted beneath (JD's Build 22 Android screenshot).
        auto frame = std::make_unique<Frame>(id_ + "-menu-frame", 1.0f, 1.0f);
        frame->setElastic(true);
        frame->setTraceBorder(true);
        frame->padding = Edges(4.0f);
        // Design-fills the menu popup; all-edge anchors keep it
        // filling when a later open sizes the frame for a
        // different item count.
        frame->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
        menuFrame_ = frame.get();
        auto list = std::make_unique<ListBox>(id_ + "-list");
        menuList_ = list.get();
        // Design-fills the menu frame, through the same anchor
        // discipline, so the options are homed inside the panel
        // on its 4-unit pad at every item count.
        menuList_->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
        menuList_->onSelect = [this](ListBox&, int idx) {
            // Close FIRST, fire the app intent second (review
            // CORE-H5): the app callback may switch roots or destroy
            // widgets, and it must never run while the menu is still
            // on the popup stack.
            setSelectedIndex(idx);
            closeMenu();
            if (onSelect) onSelect(*this, idx);
        };
        menu_->onClose = [this](Popup&) { dropState(WS_OPEN); };
        frame->addChild(std::move(list));
        menu_->addChild(std::move(frame));
    }
    // The panel's fill is the theme's panel tone, opaque: the
    // Style token carries alpha 1 and the frame's fill passes
    // through the draw params' opacity like every other fill,
    // so at the house uiOpacity of 1 nothing beneath the panel
    // shows through anywhere inside it. Refreshed per open,
    // the way Tooltip::showAt refreshes its own.
    menuFrame_->setBackground(ctx.style().panelBg);
    menuList_->items() = items_;
    menuList_->setSelectedIndex(selected_);
    float rowH = ctx.style().rowHeight;
    // Menu height is capped at 12 visible rows (review CORE-L3);
    // the ListBox scrolls to the rest. This cap is the current
    // contract (also noted in STATUS).
    const std::size_t visibleRows = std::min(items_.size(), std::size_t{12});
    float h = static_cast<float>(visibleRows) * rowH + 8.0f;
    // The menu is a popup: it lives in TREE coordinates, so the
    // anchor comes from treeRect() - inside a Frame, rect_ is
    // design coordinates and the menu floated off by the frame's
    // offset and scale (JD's screenshot, build 18 fixes FRM-2's
    // last unconverted opener). The 2px gap stays in tree units,
    // as the contract reads. The menu's own contents keep design
    // metrics (row height from the context style) - popups are
    // unscaled by design, the same as the demo's modal.
    const Rect tr = treeRect();
    menu_->setFrame({tr.x, tr.y + tr.h + 2.0f, tr.w, h});
    ctx.openPopup(menu_.get());
    raiseState(WS_OPEN);
}

void ComboBox::closeMenu() {
    if (menu_ && ctx_) ctx_->closePopup(menu_.get());
    dropState(WS_OPEN);
}

bool ComboBox::handlePointer(const PointerEvent& e) {
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Up && rect_.contains(e.x, e.y)) {
        if (isOpen()) closeMenu();
        else if (ctx_) openMenu(*ctx_);
        else if (onDropDown) onDropDown(*this);
        return true;
    }
    return e.type == PointerEvent::Type::Down;
}

bool ComboBox::handleKey(const KeyEvent& e) {
    if (!e.down) return false;
    if (e.key == KEY_ENTER || e.ch == U' ') {
        if (!isOpen() && ctx_) { openMenu(*ctx_); return true; }
        return false;
    }
    if (!isOpen() && !items_.empty()) {
        if (e.key == KEY_DOWN) {
            int n = std::min(selected_ + 1, static_cast<int>(items_.size()) - 1);
            if (n != selected_) { setSelectedIndex(n); if (onSelect) onSelect(*this, n); }
            return true;
        }
        if (e.key == KEY_UP) {
            int n = std::max(selected_ - 1, 0);
            if (n != selected_) { setSelectedIndex(n); if (onSelect) onSelect(*this, n); }
            return true;
        }
    }
    return false;
}

void ComboBox::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    dl.fillRect(rect_, p.fill(style.controlBg), style.cornerRadius);
    dl.polyline(rectOutline(rect_), (this == p.focused || isOpen()) ? style.tracePurple
                                                                    : style.border,
                style.borderWidth);
    TextStyle ts = effectiveTextStyle(style);
    // The field text seats in the padded box, short of the
    // chevron's reserve at the right (build 19): the text box
    // runs from the pad to 20 units off the right edge - the
    // chevron's ink (rect_.w - 18 .. rect_.w - 10) keeps its own
    // clearance past it - and the text anchors per the alignment
    // setting (Center by default).
    const float pad = textPad();
    const float textBoxW = std::max(0.0f, rect_.w - pad - 20.0f);
    std::string shown = ellipsizeText(selectedText(), ts, metrics, textBoxW);
    float tw = metrics.measure(shown, ts);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    dl.textRun({rect_.x + pad + alignOffset(tw, textBoxW),
                rect_.y + (rect_.h - ts.size) * 0.5f, tw, ts.size},
               shown, ts, p.fill(enabled() ? ts.color : style.textMuted), fm.ascent);
    // Chevron at the right edge.
    float cx = rect_.x + rect_.w - 14.0f;
    float cy = rect_.y + rect_.h * 0.5f;
    dl.line({cx - 4.0f, cy - 2.0f}, {cx + 4.0f, cy - 2.0f}, style.textMuted);
    dl.line({cx + 4.0f, cy - 2.0f}, {cx, cy + 3.0f}, style.textMuted);
    dl.line({cx, cy + 3.0f}, {cx - 4.0f, cy - 2.0f}, style.textMuted);
    for (auto& c : children_) c->draw(dl, p);
}

// ------------------------------------------------------------- animation

namespace anim {
namespace {
float clamp01(float t) {
    if (!(t >= 0.0f)) return 0.0f; // NaN-safe, like clampf
    return t > 1.0f ? 1.0f : t;
}
} // namespace

float linear(float t) { return clamp01(t); }

float easeInOutCubic(float t) {
    t = clamp01(t);
    if (t < 0.5f) return 4.0f * t * t * t;
    float u = -2.0f * t + 2.0f;
    return 1.0f - (u * u * u) * 0.5f;
}

float easeOutCubic(float t) {
    t = clamp01(t);
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}
} // namespace anim

// ------------------------------------------------------------ BootWindow

BootWindow::BootWindow(std::string id, std::string title)
    : Window(std::move(id), std::move(title)) {
    auto panel = std::make_unique<Panel>("boot-panel");
    panel->dir = LayoutDir::Column;
    panel->padding = Edges(14.0f);
    panel->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
    panel->setBackground(Color{0, 0, 0, 0});
    auto status = std::make_unique<Label>("boot-status", statusText_);
    status->margin = Edges(0.0f, 6.0f);
    status_ = status.get();
    panel->addChild(std::move(status));
    auto bar = std::make_unique<ProgressBar>("boot-bar", 0.0f);
    bar->fixedH = 16.0f;
    bar->margin = Edges(0.0f, 6.0f);
    bar_ = bar.get();
    panel->addChild(std::move(bar));
    auto pct = std::make_unique<Label>("boot-percent", "0%");
    percent_ = pct.get();
    panel->addChild(std::move(pct));
    addChild(std::move(panel));
}

void BootWindow::setStatusText(const std::string& t) {
    statusText_ = t;
    if (stage() == BootStage::Loading && status_) status_->setText(t);
}

void BootWindow::setProgress(float f) {
    progress_ = clampf(f, 0.0f, 1.0f);
    refreshReadout();
}

Rect BootWindow::startFrame() const {
    // A small box centered on the target's center: the frame the
    // expansion grows out of.
    float w = std::min(380.0f, target_.w * 0.42f);
    float h = std::min(190.0f, target_.h * 0.38f);
    float cx = target_.x + target_.w * 0.5f;
    float cy = target_.y + target_.h * 0.5f;
    return {cx - w * 0.5f, cy - h * 0.5f, w, h};
}

Rect BootWindow::currentFrame() const {
    if (!hasTarget()) return rect_;
    switch (stage()) {
        case BootStage::Loading:
        case BootStage::Complete:
            return startFrame();
        case BootStage::Expanding: {
            float t = expansionDuration > 0.0f
                          ? anim::easeInOutCubic(expandT_ / expansionDuration)
                          : 1.0f;
            Rect s = startFrame();
            return {s.x + (target_.x - s.x) * t, s.y + (target_.y - s.y) * t,
                    s.w + (target_.w - s.w) * t, s.h + (target_.h - s.h) * t};
        }
        case BootStage::Done:
            return target_;
    }
    return target_;
}

void BootWindow::refreshReadout() {
    if (bar_) bar_->setFraction(progress_);
    if (percent_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%",
                      static_cast<int>(std::lround(progress_ * 100.0f)));
        percent_->setText(buf);
    }
}

void BootWindow::relayout() {
    if (!hasTarget()) return;
    setFrame(currentFrame());
    Window::layout(target_, lastStyle());
}

void BootWindow::layout(Rect r, const Style& style) {
    if (stage() == BootStage::Done) {
        // Done: a plain Window (round-2 review C8). The frame the
        // expansion landed on (or a later title/resize drag set)
        // sticks; re-layouts no longer snap back to the target.
        Window::layout(r, style);
        return;
    }
    if (!hasTarget()) {
        if (r.w <= 0.0f || r.h <= 0.0f) {
            // A degenerate first layout carries no target
            // information (round-2 review C7, the CORE-H7 trap):
            // defer the capture to the first non-degenerate layout
            // and lay out as a plain window this once.
            Window::layout(r, style);
            return;
        }
        target_ = r;
        writeState(kHasTargetBit, true);
    }
    setFrame(currentFrame());
    Window::layout(r, style);
}

void BootWindow::tick(float dtSeconds) {
    if (stage() == BootStage::Done) return;
    float dt = (dtSeconds > 0.0f) ? dtSeconds : 0.0f; // NaN/negative safe
    switch (stage()) {
        case BootStage::Loading:
            if (autoRate > 0.0f) setProgress(progress_ + autoRate * dt);
            if (progress_ >= 1.0f) {
                setStage(BootStage::Complete);
                holdT_ = 0.0f;
                if (status_) status_->setText(readyText_);
            }
            break;
        case BootStage::Complete:
            holdT_ += dt;
            if (holdT_ >= completeHold) {
                setStage(BootStage::Expanding);
                expandT_ = 0.0f;
            }
            break;
        case BootStage::Expanding:
            expandT_ += dt;
            if (expansionDuration <= 0.0f || expandT_ >= expansionDuration) {
                setStage(BootStage::Done);
                relayout();
                if (onBootComplete && !completeFired()) {
                    setCompleteFired(true);
                    onBootComplete(*this);
                }
                return;
            }
            break;
        case BootStage::Done:
            break;
    }
    relayout();
}

void BootWindow::reset() {
    progress_ = 0.0f;
    setStage(BootStage::Loading);
    holdT_ = 0.0f;
    expandT_ = 0.0f;
    setCompleteFired(false);
    if (status_) status_->setText(statusText_);
    refreshReadout();
    relayout();
}

bool BootWindow::handlePointer(const PointerEvent& e) {
    if (stage() != BootStage::Done) return true; // the loading gate
    return Window::handlePointer(e);
}

void BootWindow::draw(DrawList& dl, const DrawParams& params) const {
    DrawParams p = params;
    if (stage() == BootStage::Expanding) {
        // Content fades in as the frame grows.
        float t = expansionDuration > 0.0f
                      ? anim::easeInOutCubic(expandT_ / expansionDuration)
                      : 1.0f;
        p.opacity = params.opacity * (0.45f + 0.55f * t);
    }
    Window::draw(dl, p);
}

// ------------------------------------------------------- SysterianDecoder

namespace {
// The Systerian numeral code-point base (U+F0000 + value), the
// mapping owned by the font layer (SYSTERIAN_BASE in litho_font.h).
// Repeated here as a constant because the layering runs the other
// way: litho_font.h builds on this header, not vice versa.
constexpr char32_t kDecoderSysterianBase = 0xF0000;

// splitmix64-style finalizer over (seed, position, step): the
// scramble shown at a position is a pure function of these three,
// so any tick sequence that reaches the same step shows the same
// glyph - no PRNG state, no frame-rate dependence, and tests can
// pin the exact draw list for a given seed.
std::uint64_t decoderScramble(std::uint64_t seed, std::uint64_t pos,
                              std::uint64_t step) {
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ull * (pos + 1ull) +
                      0xBF58476D1CE4E5B9ull * (step + 1ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
} // namespace

void SysterianDecoder::setText(const std::string& t) {
    text_ = t;
    cps_.clear();
    lockIndexOf_.clear();
    lockableCount_ = 0;
    std::size_t i = 0;
    while (i < text_.size()) {
        char32_t cp = nextCodePoint(text_, i);
        cps_.push_back(cp);
        if (cp == U' ') {
            lockIndexOf_.push_back(-1);
        } else {
            lockIndexOf_.push_back(lockableCount_++);
        }
    }
    start(); // a fresh text decodes from t=0, even mid-decode
}

void SysterianDecoder::start() {
    elapsed_ = 0.0f;
    setCompleteFired(false);
}

std::uint64_t SysterianDecoder::flickerStep() const {
    if (!(flickerInterval > 0.0f)) return 0; // rotation frozen at step 0
    // The +1e-4 step epsilon: accumulated float time can land a
    // hair under an exact interval multiple (0.24f / 0.12f reads
    // 1.9999999), and which side of the boundary a sum lands on
    // must not depend on how the shell sliced its ticks - that
    // is what frame-rate independent means here. The epsilon is
    // worth ~5 microseconds of schedule, far under one step.
    float s = std::floor(elapsed_ / flickerInterval + 1e-4f);
    if (!(s > 0.0f)) return 0;
    return static_cast<std::uint64_t>(s);
}

int SysterianDecoder::lockedCount() const {
    if (lockableCount_ == 0) return 0;
    if (!(lockInterval > 0.0f)) return elapsed_ > 0.0f ? lockableCount_ : 0;
    float n = std::floor(elapsed_ / lockInterval + 1e-4f); // see flickerStep
    if (!(n > 0.0f)) return 0;
    return n >= static_cast<float>(lockableCount_) ? lockableCount_
                                                   : static_cast<int>(n);
}

void SysterianDecoder::tick(float dtSeconds) {
    if (completeFired()) return; // done: further ticks are no-ops
    float dt = (dtSeconds > 0.0f) ? dtSeconds : 0.0f; // NaN/negative safe
    // decodeSpeed scales the time at the tick boundary: the
    // elapsed clock accrues dt * decodeSpeed, and everything
    // downstream (flicker step, lock count, the scramble hash)
    // stays the same pure function of elapsed_ it always was.
    elapsed_ += dt * decodeSpeed_;
    if (complete()) {
        setCompleteFired(true);
        if (onDecodeComplete) onDecodeComplete(*this);
    }
}

void SysterianDecoder::setElapsed(float seconds) {
    // NaN fails the comparison and reads as 0, the same guard
    // form as tick's dt. decodeSpeed is not applied: the
    // driving clock (the boot bar) is the whole pace here.
    elapsed_ = (seconds > 0.0f) ? seconds : 0.0f;
    if (!completeFired() && complete()) {
        setCompleteFired(true);
        if (onDecodeComplete) onDecodeComplete(*this);
    }
}

bool SysterianDecoder::systShown(std::size_t i, std::uint64_t step) const {
    // Step 0 is the pure-Systerian opening frame. After that the
    // position rotates: Systerian on odd (step + i), ASCII on even,
    // so neighbours never blink in unison.
    return step == 0 || ((step + static_cast<std::uint64_t>(i)) & 1ull) != 0ull;
}

// Build 14: the scramble's Systerian values come from a FIXED
// pool spread across the face's 1..9999 range, not from the
// whole range. They used to be any value (1 + h % 9999): every
// distinct value a long-running decoder showed baked a NEW cell
// into the shared 256-cell glyph atlas, the atlas never evicts,
// and a session that sat on the gallery could exhaust it - after
// which unrelated text anywhere else (JD's Build 13 web test:
// the inventory's title bar, its inner header, its item counts)
// failed glyph coverage and rendered as the rasterizer's
// missing-glyph dash bars, while text whose cells were already
// baked rendered fine. The pool bounds the numeral face's cell
// cost at kScramblePoolSize cells however long the scramble
// runs. The pool step (3571) is coprime with 9999, so the first
// 48 multiples land on 48 distinct values spread over the range
// and the scramble still reads as arbitrary numerals.
constexpr std::uint64_t kScramblePoolSize = 48;
std::uint32_t scramblePoolValue(std::uint64_t h) {
    const std::uint64_t idx = h % kScramblePoolSize;
    return 1u + static_cast<std::uint32_t>((idx * 3571u) % 9999u);
}

char32_t SysterianDecoder::shownCp(std::size_t i, std::uint64_t step) const {
    std::uint64_t h = decoderScramble(seed_, static_cast<std::uint64_t>(i), step);
    if (systShown(i, step))
        return kDecoderSysterianBase + static_cast<char32_t>(scramblePoolValue(h));
    return static_cast<char32_t>(0x21 + (h >> 32) % 94); // '!' .. '~'
}

float SysterianDecoder::desiredWidth(const Style& style) const {
    if (fixedW >= 0.0f) return fixedW;
    static const ApproxTextMetrics approx;
    TextStyle ts = effectiveTextStyle(style);
    // The footprint is the target's plus the pad on both sides
    // (build 19: the decoder's content box is its padded box;
    // it read the layout `padding` member before the setting
    // existed, and no composition ever set it).
    return approx.measure(text_, ts) + 2.0f * textPad();
}

float SysterianDecoder::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    TextStyle ts = effectiveTextStyle(style);
    return ts.size * ts.lineHeightMul + 2.0f * textPad();
}

void SysterianDecoder::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    drawBackgroundAndBorder(dl, p);
    if (cps_.empty()) return;

    TextStyle ts = effectiveTextStyle(style);
    // The run styles: locked text carries the caller's chain
    // untouched; Systerian scramble runs get the wired Systerian
    // face FIRST in their chain, so the numeral code points
    // resolve to it ahead of any general fallback.
    TextStyle systStyle = ts;
    if (systerianFont_ != FONT_INVALID) {
        systStyle.fallbacks.insert(systStyle.fallbacks.begin(), systerianFont_);
    }
    Color lockedCol = ts.color.a > 0.0f ? ts.color : style.text;
    lockedCol = p.fill(lockedCol);
    Color systCol = p.fill(style.traceBlue);
    Color scrambleCol = p.fill(style.textMuted);

    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    Rect inner = textRect();
    // Horizontal placement by the target's own alignment (the
    // alignment setting, through effectiveTextStyle): the total
    // is the target measured whole, and each position then sits
    // in a fixed cell of its target character's advance, so the
    // line's footprint is the target's footprint at all times.
    float totalW = metrics.measure(text_, ts);
    float penX = inner.x;
    if (ts.align == TextAlign::Center) penX += (inner.w - totalW) * 0.5f;
    else if (ts.align == TextAlign::Right) penX += inner.w - totalW;
    const float y = inner.y;

    const std::uint64_t step = flickerStep();
    const int locked = lockedCount();
    auto resolvedAdvance = [&](char32_t cp) {
        FontId f = ts.font;
        if (!metrics.hasGlyph(f, cp)) {
            for (FontId fb : ts.fallbacks) {
                if (metrics.hasGlyph(fb, cp)) { f = fb; break; }
            }
        }
        return metrics.glyphAdvance(cp, f, ts.size, ts.weight);
    };

    for (std::size_t i = 0; i < cps_.size(); ++i) {
        char32_t target = cps_[i];
        float adv = resolvedAdvance(target);
        if (i + 1 < cps_.size()) adv += ts.letterSpacing;
        if (target == U' ') { penX += adv; continue; } // spaces carry no glyph

        const bool isLocked = lockIndexOf_[i] < locked;
        char32_t shown;
        const TextStyle* runStyle = &ts;
        Color col = lockedCol;
        if (isLocked) {
            shown = target;
        } else if (systShown(i, step)) {
            shown = shownCp(i, step);
            runStyle = &systStyle;
            col = systCol;
        } else {
            shown = shownCp(i, step);
            col = scrambleCol;
        }
        std::string s;
        utf8Encode(shown, s);
        float w = metrics.measure(s, *runStyle);
        dl.textRun({penX, y, w, ts.size}, s, *runStyle, col, fm.ascent);
        penX += adv;
    }
}

float bootLogElapsedFor(float progress, const int* lockableCounts,
                        std::size_t lineCount, std::size_t line,
                        float lockInterval) {
    if (lockableCounts == nullptr || line >= lineCount) return 0.0f;
    const int mine = lockableCounts[line];
    if (mine <= 0) return 0.0f; // nothing to lock: complete at any elapsed
    // Segment boundaries in double: the tiling is exact integer
    // ratios, and a float progress landing on a boundary must
    // resolve to the finished side of it (the decoder's own
    // +1e-4 step epsilon absorbs the remaining residue).
    double total = 0.0, before = 0.0;
    for (std::size_t i = 0; i < lineCount; ++i) {
        if (lockableCounts[i] > 0) {
            total += lockableCounts[i];
            if (i < line) before += lockableCounts[i];
        }
    }
    if (!(total > 0.0)) return 0.0f;
    const float p = clampf(progress, 0.0f, 1.0f); // NaN reads as 0
    const double segStart = before / total;
    const double segEnd = (before + static_cast<double>(mine)) / total;
    double local = (static_cast<double>(p) - segStart) / (segEnd - segStart);
    if (!(local > 0.0)) return 0.0f;
    if (local > 1.0) local = 1.0;
    return static_cast<float>(local * static_cast<double>(mine)) * lockInterval;
}

// ----------------------------------------------------------- ProgressBar

ProgressBar::ProgressBar(std::string id, float fraction) : Frame(std::move(id), Frame::InertFrameTag{}) {
    setFraction(fraction);
}

void ProgressBar::setFraction(float f) {
    fraction_.store(clampf(f, 0.0f, 1.0f), std::memory_order_relaxed);
}

float ProgressBar::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    return style.rowHeight;
}

void ProgressBar::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    dl.fillRect(rect_, p.fill(style.controlBg), style.cornerRadius);
    float f = fraction();
    if (f > 0.0f) {
        Rect bar = rect_;
        bar.w = rect_.w * f;
        dl.fillRect(bar, style.traceBlue, style.cornerRadius);
    }
    dl.polyline(rectOutline(rect_), style.border, style.borderWidth);
    if (showPercent()) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(f * 100.0f)));
        TextStyle ts = effectiveTextStyle(style);
        float tw = metrics.measure(buf, ts);
        FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
        // The percent seats in the padded box per the alignment
        // setting (build 19; Center by default, the historical
        // centered readout). The bar's fill above is the widget's
        // box-filling layer and still spans the whole rect.
        const Rect seat = textRect();
        dl.textRun({seat.x + alignOffset(tw, seat.w),
                    rect_.y + (rect_.h - ts.size) * 0.5f, tw, ts.size},
                   buf, ts, p.fill(ts.color), fm.ascent);
    }
    for (auto& c : children_) c->draw(dl, p);
}

// ----------------------------------------------------------------- Image

float Image::desiredWidth(const Style& style) const {
    if (fixedW >= 0.0f) return fixedW;
    if (naturalW_ >= 0.0f) return naturalW_;
    return Widget::desiredWidth(style);
}

float Image::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    if (naturalH_ >= 0.0f) return naturalH_;
    return Widget::desiredHeight(style);
}

void Image::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    drawBackgroundAndBorder(dl, p);
    // The effective opacity rides the command as imageAlpha and
    // multiplies texel alpha at the back end, so images fade with
    // windows/themes exactly like fills (p.fill) and text.
    if (textureId_ >= 0) dl.imageQuad(rect_, textureId_, p.opacity);
    for (auto& c : children_) c->draw(dl, p);
}

// ------------------------------------------------------------ VirtualList

float VirtualList::maxScroll() const {
    return std::max(0.0f, static_cast<float>(totalCount()) * rowH_ - rect_.h);
}

int VirtualList::firstVisibleIndex() const {
    if (rowH_ <= 0.0f) return 0;
    return std::min(totalCount(), static_cast<int>(scrollY_ / rowH_));
}

void VirtualList::rebind(const Style& style) {
    rowH_ = richFn ? richRowH : style.rowHeight;
    scrollY_ = clampf(scrollY_, 0.0f, maxScroll());
    int count = totalCount();
    int firstVis = firstVisibleIndex();
    int first = std::max(0, firstVis - kOverscan);
    int visible = rowH_ > 0.0f ? static_cast<int>(std::ceil(rect_.h / rowH_)) + 1 : 0;
    int last = std::min(count, firstVis + visible + kOverscan);
    pool_.clear();
    for (int i = first; i < last; ++i) {
        RowBinding b;
        b.index = i;
        b.text = textFn ? textFn(i) : "";
        if (richFn) richFn(i, b.rich);
        pool_.push_back(std::move(b));
    }
    firstBound_ = first;
}

float VirtualList::desiredHeight(const Style& style) const {
    if (fixedH >= 0.0f) return fixedH;
    return style.rowHeight * 6.0f;
}

void VirtualList::layout(Rect r, const Style& style) {
    noteStyle(style);
    rect_ = r;
    lastLayoutW_ = r.w;
    rebind(style);
}

bool VirtualList::handlePointer(const PointerEvent& e) {
    if (e.type == PointerEvent::Type::Wheel) {
        scrollY_ -= e.wheelDelta * 24.0f;
        layout(rect_, lastStyle());
        return true;
    }
    if (e.button != 0) return false;
    if (e.type == PointerEvent::Type::Up && rect_.contains(e.x, e.y)) {
        int idx = static_cast<int>((e.y - rect_.y + scrollY_) / rowH_);
        if (idx >= 0 && idx < totalCount()) {
            selected_ = idx;
            if (onSelect) onSelect(*this, idx);
        }
        return true;
    }
    return e.type == PointerEvent::Type::Down;
}

bool VirtualList::handleKey(const KeyEvent& e) {
    if (!e.down) return false;
    int count = totalCount();
    if (count == 0) return false;
    if (e.key == KEY_DOWN || e.key == KEY_UP) {
        int n = selected_ + (e.key == KEY_DOWN ? 1 : -1);
        n = std::max(0, std::min(n, count - 1));
        if (n != selected_) {
            selected_ = n;
            if (onSelect) onSelect(*this, n);
            // Keep the selection visible: scroll the window to it.
            float top = static_cast<float>(n) * rowH_;
            if (top < scrollY_) scrollY_ = top;
            if (top + rowH_ > scrollY_ + rect_.h) scrollY_ = top + rowH_ - rect_.h;
            layout(rect_, lastStyle());
        }
        return true;
    }
    return false;
}

void VirtualList::draw(DrawList& dl, const DrawParams& params) const {
    if (!visible()) return;
    DrawParams p = localParams(params);
    const Style& style = *p.style;
    const ITextMetrics& metrics = *p.metrics;
    const_cast<VirtualList*>(this)->rebind(style);
    drawBackgroundAndBorder(dl, p);
    dl.pushClip(rect_);
    TextStyle ts = effectiveTextStyle(style);
    FontMetrics fm = metrics.fontMetrics(ts.font, ts.size);
    for (auto& b : pool_) {
        float y = rect_.y + static_cast<float>(b.index) * rowH_ - scrollY_;
        if (y + rowH_ < rect_.y || y > rect_.y + rect_.h) continue;
        if (b.index == selected_)
            dl.fillRect({rect_.x, y, rect_.w, rowH_}, p.fill(style.selection), 0.0f);
        if (richFn) {
            // Rich row: avatar quad on the left, author line (bold,
            // author color), span-built message line under it. The
            // message line is single-line and clips at the list edge;
            // wrapping inside a virtual row is not part of the
            // contract (rows are fixed-height by design).
            const RichRowData& r = b.rich;
            // Row content seats from the padded box's left edge
            // (build 19): avatar, then the text column 8 past it.
            // (The vertical rhythm is the scroll surface's own,
            // as in TreeView / ListBox.)
            dl.imageQuad({rect_.x + textPad(), y + 5.0f, 36.0f, 36.0f},
                         r.avatarTexture, p.opacity);
            const float tx = rect_.x + textPad() + 44.0f;
            TextStyle ats = ts;
            ats.weight = 700;
            Color ac = r.authorColor.a > 0.0f ? r.authorColor : ts.color;
            float aw = metrics.measure(r.author, ats);
            dl.textRun({tx, y + 3.0f, aw, ats.size}, r.author, ats, p.fill(ac), fm.ascent);
            float sx = tx;
            const float sy = y + 3.0f + ts.size + 5.0f;
            for (auto& sp : r.spans) {
                TextStyle ss = sp.style ? *sp.style : ts;
                if (ss.color.a <= 0.0f) ss.color = ts.color;
                float sw = metrics.measure(sp.text, ss);
                FontMetrics sfm = metrics.fontMetrics(ss.font, ss.size);
                dl.textRun({sx, sy, sw, ss.size}, sp.text, ss, p.fill(ss.color), sfm.ascent);
                // Span decorations, same rule as Label (markdown
                // links / strike spans in chat rows).
                if (ss.underline)
                    dl.line({sx, sy + sfm.ascent + 2.0f},
                            {sx + sw, sy + sfm.ascent + 2.0f}, p.fill(ss.color), 1.0f);
                if (ss.strikethrough) {
                    float ly = sy + sfm.ascent * 0.62f;
                    dl.line({sx, ly}, {sx + sw, ly}, p.fill(ss.color), 1.0f);
                }
                sx += sw;
            }
            continue;
        }
        float tw = metrics.measure(b.text, ts);
        dl.textRun({rect_.x + textPad() + alignOffset(tw, rect_.w - 2.0f * textPad()),
                    y + (rowH_ - ts.size) * 0.5f, tw, ts.size},
                   b.text, ts, p.fill(ts.color), fm.ascent);
    }
    dl.popClip();
    for (auto& c : children_) c->draw(dl, p);
}

// ----------------------------------------------------------------- Popup

Popup::Popup(std::string id, Flavor flavor) : Frame(std::move(id), Frame::InertFrameTag{}), flavor_(flavor) {}

void Popup::layout(Rect r, const Style& style) {
    if (hasFrame()) r = frame_;
    Widget::layout(r, style);
}

void Popup::close() {
    if (ctx_) ctx_->closePopup(this);
}

Popup::~Popup() {
    // Unregister from the owning context (review CORE-H1): the stack
    // never keeps a dangling pointer to a destroyed popup, and the
    // context repairs its interaction pointers by comparison. The
    // popup's children are still alive here (they die with the base
    // Widget), so the descendant walk in forgetPopup is safe.
    if (ctx_) {
        UiContext* ctx = ctx_;
        ctx_ = nullptr;
        ctx->forgetPopup(this);
    }
}

// --------------------------------------------------------------- Tooltip

Tooltip::Tooltip(std::string id, std::string text)
    : Popup(std::move(id), Flavor::Overlay) {
    setTraceBorder(true);
    padding = Edges(8.0f, 4.0f);
    auto label = std::make_unique<Label>(id_ + "-label", text);
    label_ = label.get();
    // The label design-fills the tip; anchor it to all four edges so
    // it keeps filling when showAt sizes a different frame later.
    label_->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
    addChild(std::move(label));
}

void Tooltip::showAt(UiContext& ctx, Rect anchor) {
    const Style& style = ctx.style();
    ApproxTextMetrics approx;
    TextStyle ts = label_->textStyle ? *label_->textStyle : style.bodyText;
    float w = approx.measure(text(), ts) + padding.l + padding.r + 2.0f;
    float h = approx.lineHeight(ts) + padding.t + padding.b + 2.0f;
    float x = std::max(0.0f, anchor.x + anchor.w * 0.5f - w * 0.5f);
    float y = anchor.y - h - 4.0f;
    if (y < 0.0f) y = anchor.y + anchor.h + 4.0f;
    // 2026-11-08: keep the whole tip inside the viewport. Before, a tip
    // anchored near the right or bottom edge hung off-screen (only the
    // left edge was clamped and only the top had the flip).
    if (Widget* root = ctx.root()) {
        const Rect vp = root->rect();
        if (vp.w > 0) x = std::min(x, std::max(vp.x, vp.x + vp.w - w));
        if (vp.h > 0) y = std::min(y, std::max(vp.y, vp.y + vp.h - h));
    }
    setFrame({x, y, w, h});
    setBackground(style.panelBg); // theme's panel tone, refreshed per show
    ctx.openPopup(this);
}

// -------------------------------------------------------------- UiContext

const ITextMetrics& UiContext::metrics() const {
    return metrics_ ? *metrics_ : static_cast<const ITextMetrics&>(approx_);
}

float UiContext::scaleFactor() const {
    const float s = style_.uiScale;
    return (s > 0.0f && std::isfinite(s)) ? s : 1.0f;
}

void UiContext::layout(Rect viewport) {
    if (!root_) return;
    const float s = scaleFactor();
    if (s != 1.0f) {
        viewport.x /= s;
        viewport.y /= s;
        viewport.w /= s;
        viewport.h /= s;
    }
    root_->layout(viewport, style_);
}

static Widget* hitRecurse(Widget* w, float x, float y) {
    if (!w->visible() || !w->enabled()) return nullptr;
    const bool inside = w->rect().contains(x, y);
    if (inside) {
        // Grip-zone precedence: inside a Window's outer grip band the
        // window's resize chrome wins over its children, so the hit is
        // the window itself and no child in the band is consulted.
        // Children keep the whole interior; only the kResizeGrip-wide
        // band at the frame's edge belongs to the chrome.
        if (auto* win = dynamic_cast<Window*>(w)) {
            if (win->gripEdgesAt(x, y) != 0) return w;
        }
    }
    // Clipping parents (round-2 review C9): a widget whose draw
    // PushClips its children to its own rect (ScrollArea, TreeView,
    // ListBox, VirtualList, multi-line TextField) shows nothing of
    // them outside the rect, so they are not hittable there either;
    // clipped-away content must not steal clicks from the visible
    // siblings drawn over that spot.
    if (!inside && w->clipsChildren()) return nullptr;
    // Children are consulted even when this widget's own rect does
    // not contain the point (review CORE-M7): a child that overflows
    // its parent is drawn there, so it is hittable there. This widget
    // itself is a hit only when the point is inside it.
    // Frame children live in DESIGN space (see Frame): the point
    // is mapped through the frame before its children are
    // consulted, and a point in the frame's letterbox (inside
    // the frame, outside its content box) never reaches them -
    // the hit there is the Frame itself.
    float cx = x, cy = y;
    if (auto* fr = dynamic_cast<Frame*>(w); fr && fr->frameEngaged()) {
        if (!fr->pointInContent(x, y)) return inside ? w : nullptr;
        const Vec2 d = fr->toDesign({x, y});
        // Scrollbar chrome (build 20): a point on a scrollable
        // Frame's scrollbar strip is the Frame's, never the
        // content painted under it - the same chrome-over-
        // children precedence as the Window grip above. (The
        // strip is inside the content box, so this only runs
        // for points the content would otherwise see.)
        if (fr->scrollbarHit(d.x, d.y)) return w;
        cx = d.x;
        cy = d.y;
    }
    const auto& kids = w->children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        if (Widget* hit = hitRecurse(it->get(), cx, cy)) return hit;
    }
    return inside ? w : nullptr;
}

Popup* UiContext::topInteractivePopup() const {
    for (auto it = popupStack_.rbegin(); it != popupStack_.rend(); ++it) {
        if ((*it)->flavor() != Popup::Flavor::Overlay) return *it;
    }
    return nullptr;
}

void UiContext::applyFocus(Widget* w) {
    if (focus_ == w) return;
    if (focus_) {
        // Lifetime audit F-2: the old holder may have been destroyed
        // without any repair reaching us (an owner resetting its
        // unique_ptr, a facade subtree drained by destroyLater before
        // this context existed in the path). Never dereference a
        // tombstoned holder: record it for the lazy bit reap, exactly
        // as setRoot does, and move on.
        if (widgetTombstoned(focus_)) staleFocusBit_ = focus_;
        else focus_->dropState(WS_FOCUSED);
    }
    focus_ = w;
    if (focus_) focus_->raiseState(WS_FOCUSED);
}

void UiContext::applyHover(Widget* w) {
    if (hover_ == w) return;
    if (hover_) {
        // Lifetime audit F-1: hover_ is focus_'s twin and got none of
        // its repairs... a plain widget destroyed via the facade's
        // destroyLater drain leaves hover_ dangling, and every pointer
        // Move used to write WS_HOVERED through the freed widget here.
        // Same discipline as applyFocus: tombstoned holders are never
        // dereferenced, only recorded for reapStaleBits.
        if (widgetTombstoned(hover_)) staleHoverBit_ = hover_;
        else hover_->dropState(WS_HOVERED);
    }
    hover_ = w;
    if (hover_) hover_->raiseState(WS_HOVERED);
}

UiContext::~UiContext() {
    // Detach, never destroy (review C3): popups and context clients
    // (ComboBox) that outlive the context get their back-pointer
    // nulled, so a later close() / click on them is a safe no-op
    // (or the onDropDown fallback) instead of a dangling call.
    for (Popup* p : popupStack_) {
        if (p->ctx_ == this) p->ctx_ = nullptr;
        p->markOpen(false);
    }
    popupStack_.clear();
    for (Widget* w : contextClients_) w->detachContext(this);
    contextClients_.clear();
}

void UiContext::registerContextClient(Widget* w) {
    if (!w) return;
    for (Widget* q : contextClients_)
        if (q == w) return;
    contextClients_.push_back(w);
}

void UiContext::unregisterContextClient(Widget* w) {
    contextClients_.erase(std::remove(contextClients_.begin(), contextClients_.end(), w),
                          contextClients_.end());
}

void UiContext::setRoot(Widget* root) {
    // Drain the popup stack while the popups are alive: each close
    // fires its onClose and repairs interaction pointers (CORE-H2).
    while (!popupStack_.empty()) closePopup(popupStack_.back());
    // Compare-and-clear ONLY: the old tree may already be destroyed,
    // so its widgets are never dereferenced here. The focus/hover
    // state BITS on the old widgets therefore cannot be dropped
    // either; the holders are recorded and their bits are reaped
    // lazily when a later tree walk proves them alive (review C4,
    // see reapStaleBits), which keeps the bits honest for
    // kept-alive trees (UiStateMachine) without ever dereferencing
    // a possibly-dead widget here.
    if (focus_) staleFocusBit_ = focus_;
    if (hover_) staleHoverBit_ = hover_;
    focus_ = nullptr;
    hover_ = nullptr;
    capture_ = nullptr;
    root_ = root;
}

void UiContext::reapStaleBits(Widget* w) const {
    if (!w || (!staleFocusBit_ && !staleHoverBit_)) return;
    if (w == staleFocusBit_) {
        // Focus moved on (or never came back): the bit must not
        // outlive the focus it described. When this widget IS the
        // current focus the bit is correct and stays.
        if (focus_ != w) w->dropState(WS_FOCUSED);
        staleFocusBit_ = nullptr;
    }
    if (w == staleHoverBit_) {
        if (hover_ != w) w->dropState(WS_HOVERED);
        staleHoverBit_ = nullptr;
    }
    for (auto& c : w->children()) reapStaleBits(c.get());
}

bool UiContext::isInTree(const Widget* w, const Widget* root) const {
    for (const Widget* p = w; p != nullptr; p = p->parent()) {
        if (p == root) return true;
    }
    return false;
}

bool UiContext::focusChainValid(const Widget* w) const {
    if (!w) return false;
    bool inSomeTree = (root_ != nullptr && isInTree(w, root_));
    if (!inSomeTree) {
        for (Popup* p : popupStack_) {
            if (isInTree(w, p)) { inSomeTree = true; break; }
        }
    }
    if (!inSomeTree) return false;
    for (const Widget* p = w; p != nullptr; p = p->parent()) {
        if (!p->visible() || !p->enabled()) return false;
    }
    return true;
}

void UiContext::repairInteractionPointers(const Widget* dead) {
    if (!dead) return;
    if (focus_ && isInTree(focus_, dead)) applyFocus(nullptr);
    if (hover_ && isInTree(hover_, dead)) applyHover(nullptr);
    if (capture_ && isInTree(capture_, dead)) capture_ = nullptr;
}

void UiContext::forgetPopup(Popup* p) {
    popupStack_.erase(std::remove(popupStack_.begin(), popupStack_.end(), p),
                      popupStack_.end());
    repairInteractionPointers(p);
    // A modal destroyed while open gets the same saved-focus
    // restore closePopup performs (round-2 review C5): destruction
    // must not silently lose the focus the modal was holding for
    // the shell. (No onClose fires here: the popup is dying.)
    restoreModalFocus(p);
    if (p->ctx_ == this) p->ctx_ = nullptr;
}

void UiContext::restoreModalFocus(Popup* p) {
    if (p->flavor() != Popup::Flavor::Modal) return;
    Widget* saved = p->savedFocus_;
    p->savedFocus_ = nullptr;
    // Restore only when nothing else took focus since, the saved
    // widget is not inside the popup itself, and its chain is still
    // valid (review CORE-H4's rule, shared by both close paths).
    // Lifetime audit F-3: widgetAlive runs FIRST... isInTree and
    // focusChainValid walk saved->parent() chains, which is a member
    // read on the saved widget itself; the widget that held focus at
    // modal open (e.g. the menu Play button) can be destroyed while
    // the modal is open (a resize rebuilds its tree), and the restore
    // used to walk the freed widget before any check. widgetAlive is
    // pointer-only, matching the dispatchKey discipline.
    if (!focus_ && saved && saved != p && widgetAlive(saved) &&
        !isInTree(saved, p) && focusChainValid(saved))
        applyFocus(saved);
}

namespace {
// The tombstone set behind the liveness registry (see the header
// note): addresses of widgets destroyed and not since recycled by
// a new construction. A function-local static, so no static
// initialization order hazards with static widgets.
std::unordered_set<const Widget*>& tombstones() {
    static std::unordered_set<const Widget*> set;
    return set;
}
// Pointer-only subtree membership: walks the LIVE tree under
// `node` comparing child pointers; `needle` is never dereferenced.
// Only ever called on roots proven live by the tombstone check: a
// live widget's children are live by ownership, so the walk never
// touches a dead widget.
bool treeContainsPtr(const Widget* node, const Widget* needle) {
    if (node == needle) return true;
    for (auto& c : node->children())
        if (treeContainsPtr(c.get(), needle)) return true;
    return false;
}
} // namespace

void noteWidgetConstructed(const Widget* w) { tombstones().erase(w); }
void noteWidgetDestroyed(const Widget* w) { tombstones().insert(w); }
bool widgetTombstoned(const Widget* w) { return tombstones().count(w) != 0; }

bool UiContext::widgetAlive(const Widget* w) const {
    if (!w || widgetTombstoned(w)) return false;
    if (root_ && !widgetTombstoned(root_) && treeContainsPtr(root_, w)) return true;
    for (Popup* p : popupStack_)
        if (!widgetTombstoned(p) && treeContainsPtr(p, w)) return true;
    return false;
}

std::vector<Widget*> UiContext::snapshotChain(Widget* w) {
    std::vector<Widget*> chain;
    for (Widget* p = w; p != nullptr; p = p->parent()) chain.push_back(p);
    return chain;
}

Widget* UiContext::hitTest(float x, float y) const {
    const float s = scaleFactor();
    if (s != 1.0f) {
        x /= s;
        y /= s;
    }
    return hitTestLogical(x, y);
}

Widget* UiContext::hitTestLogical(float x, float y) const {
    if (Popup* top = topInteractivePopup()) {
        if (Widget* hit = hitRecurse(top, x, y)) return hit;
        // A modal topmost suppresses hit testing of the root tree
        // entirely (review CORE-H4).
        if (top->flavor() == Popup::Flavor::Modal) return nullptr;
    }
    if (!root_) return nullptr;
    return hitRecurse(root_, x, y);
}

bool UiContext::bubblePointer(Widget* target, const PointerEvent& e) {
    // Snapshot the ancestor chain BEFORE invoking any handler
    // (review CORE-H5): a handler may destroy widgets (its own
    // ancestors included), and the walk must never follow a parent
    // pointer out of a dead widget. Each snapshot entry is also
    // liveness-checked by pointer comparison before it is called
    // (round-2 review C1/C2 hardening): an entry a previous handler
    // destroyed is skipped, never dereferenced.
    std::vector<Widget*> chain = snapshotChain(target);
    // Frame coordinate mapping (see Frame): every Frame among the
    // target's ancestors holds the target in its DESIGN space, so
    // the event is mapped through those Frames (outermost first)
    // before the walk, and mapped back out as the walk ascends
    // past each Frame - handlers inside a Frame see design
    // coordinates matching their design rects, handlers at and
    // above the Frame see the tree coordinates they expect. The
    // mapping is pure math (no containment test): a captured drag
    // that leaves the content box still delivers design
    // coordinates outside the design rect, which drop-outside
    // handling inside the frame is built on. The mapping state
    // advances even past a dead snapshot entry, so the un-mapping
    // order can never drift.
    PointerEvent ev = e;
    // The un-mapping on ascent must never dereference a Frame a
    // handler destroyed (round-3 review FRM-1: the ascent used
    // to call toTree() on the snapshot entry BEFORE its liveness
    // check - a use-after-free when a deeper handler rebuilt the
    // tree and declined the event). Each applied mapping is
    // therefore snapshotted as pure values (scale + content
    // origin, the whole of what toTree computes) at pre-map
    // time, and the ascent un-maps from the snapshot.
    struct AppliedMap { const Widget* frame; float scale; Vec2 origin; };
    std::vector<AppliedMap> applied; // outermost .. innermost
    for (std::size_t i = chain.size(); i-- > 1;) {
        if (auto* fr = dynamic_cast<Frame*>(chain[i]);
            fr && fr->frameEngaged()) {
            const Vec2 d = fr->toDesign({ev.x, ev.y});
            ev.x = d.x;
            ev.y = d.y;
            // The un-mapping origin is the image of the design
            // origin under the frame's own toTree: for an
            // unscrolled frame that is exactly the content
            // rect's corner (what this snapshot used to store);
            // for a scrolled one (build 20) it carries the
            // offset, so the ascent inverts the descent exactly.
            const Vec2 org = fr->toTree({0.0f, 0.0f});
            applied.push_back(AppliedMap{fr, fr->contentScale(), org});
        }
    }
    for (Widget* w : chain) {
        if (!applied.empty() && applied.back().frame == w) {
            const AppliedMap m = applied.back();
            ev.x = m.origin.x + ev.x * m.scale;
            ev.y = m.origin.y + ev.y * m.scale;
            applied.pop_back();
        }
        if (!widgetAlive(w)) continue;
        if (w->handlePointer(ev)) return true;
    }
    return false;
}

namespace {
// First focusable widget in DFS order (visible + enabled chain), or
// nullptr. Used to give a modal popup the keyboard on open.
Widget* firstFocusableIn(Widget* w) {
    if (!w->visible() || !w->enabled()) return nullptr;
    if (w->focusable()) return w;
    for (auto& c : w->children()) {
        if (Widget* f = firstFocusableIn(c.get())) return f;
    }
    return nullptr;
}
} // namespace

void UiContext::openPopup(Popup* p) {
    if (!p) return;
    // Already on the stack: re-layout in place, never a duplicate
    // entry for closeTopPopup to trip over (review CORE-M13).
    for (Popup* q : popupStack_) {
        if (q == p) {
            Rect vp0 = root_ ? root_->rect() : Rect{0, 0, 640, 480};
            p->layout(vp0, style_);
            return;
        }
    }
    p->ctx_ = this;
    p->markOpen(true);
    popupStack_.push_back(p);
    Rect vp = root_ ? root_->rect() : Rect{0, 0, 640, 480};
    p->layout(vp, style_);
    if (p->flavor() == Popup::Flavor::Modal) {
        // A modal takes the pointer as well as the keyboard
        // (round-2 review C10): a capture earned before the modal
        // opened must not keep routing Move/Up behind it. Capture
        // pointing INSIDE the modal (impossible at open time, kept
        // for symmetry) is left alone.
        if (capture_ && !isInTree(capture_, p)) capture_ = nullptr;
        // Modal keyboard capture (review CORE-H4): save the current
        // focus and move it into the popup; closePopup restores it.
        p->savedFocus_ = focus_;
        Widget* target = firstFocusableIn(p);
        applyFocus(target ? target : p);
    }
}

void UiContext::closePopup(Popup* p) {
    if (!p) return;
    popupStack_.erase(std::remove(popupStack_.begin(), popupStack_.end(), p), popupStack_.end());
    p->markOpen(false);
    p->ctx_ = nullptr;
    // Repair capture/focus/hover pointing at the popup OR at any
    // widget inside it (review CORE-H3).
    repairInteractionPointers(p);
    // Restore the focus saved at open (review CORE-H4), when it is
    // still in a live tree and nothing else took focus since.
    restoreModalFocus(p);
    if (p->onClose) p->onClose(*p);
}

void UiContext::closeTopPopup() {
    if (!popupStack_.empty()) closePopup(popupStack_.back());
}

// ------------------------------------------------------------- z-order
// A window's z among its siblings IS its position in the parent's
// child vector: draw walks the vector forward (later = on top) and
// hit testing walks it in reverse (topmost first). Raising/lowering
// therefore just re-seats the window's unique_ptr among its siblings;
// widget pointers stay valid and no other structure changes.

namespace {
// The nearest Window at or above `w` in the parent chain, or nullptr.
Window* windowAncestor(Widget* w) {
    for (Widget* p = w; p != nullptr; p = p->parent()) {
        if (auto* win = dynamic_cast<Window*>(p)) return win;
    }
    return nullptr;
}
} // namespace

void UiContext::raiseWindow(Window* w) {
    if (!w || !w->parent()) return;
    auto& kids = w->parent()->children_;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() != w) continue;
        if (i + 1 == kids.size()) return; // already topmost
        std::unique_ptr<Widget> held = std::move(kids[i]);
        kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(i));
        kids.push_back(std::move(held));
        return;
    }
}

void UiContext::lowerWindow(Window* w) {
    if (!w || !w->parent()) return;
    auto& kids = w->parent()->children_;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() != w) continue;
        if (i == 0) return; // already bottommost
        std::unique_ptr<Widget> held = std::move(kids[i]);
        kids.erase(kids.begin() + static_cast<std::ptrdiff_t>(i));
        kids.insert(kids.begin(), std::move(held));
        return;
    }
}

bool UiContext::dispatchPointer(const PointerEvent& inEvent) {
    // Boundary conversion: the caller speaks physical pixels, the tree
    // works in logical units. Convert once, here; wheelDelta is a notch
    // count, not a length, and is deliberately not scaled.
    PointerEvent ev = inEvent;
    const float s = scaleFactor();
    if (s != 1.0f) {
        ev.x /= s;
        ev.y /= s;
    }
    const PointerEvent& e = ev;
    if (!root_ && popupStack_.empty()) return false;
    // Per-event capture revalidation (review CORE-M14): a capture
    // target that left its tree, hid, or was disabled since the Down
    // releases the capture instead of receiving events blind. The
    // liveness half is pointer-comparison only (round-2 review
    // C1): a capture target destroyed since the last event is
    // dropped without being dereferenced; focusChainValid runs
    // only on a target proven alive.
    if (capture_ && (!widgetAlive(capture_) || !focusChainValid(capture_)))
        capture_ = nullptr;
    if (e.type == PointerEvent::Type::Down && e.button == 0) {
        if (Popup* top = topInteractivePopup()) {
            Widget* hit = hitRecurse(top, e.x, e.y);
            if (!hit) {
                // Outside click: a modal swallows it; a menu popup closes
                // and swallows it. The widget beneath never sees it.
                if (top->flavor() == Popup::Flavor::Menu) closePopup(top);
                return true;
            }
            applyFocus(hit->focusable() ? hit : nullptr);
            bool handled = bubblePointer(hit, e);
            // Capture is earned by a HANDLED Down only (CORE-M14),
            // and the hit widget is revalidated by pointer
            // comparison AFTER the bubble (round-2 review C1): a
            // handler that destroyed the tree mid-dispatch (the
            // CORE-H5 pattern) must not leave capture_ dangling
            // for the next event's revalidation to dereference.
            if (handled) capture_ = widgetAlive(hit) ? hit : nullptr;
            return handled;
        }
        Widget* hit = hitTestLogical(e.x, e.y);
        // Pointer down raises the window it lands in (focus follows
        // the same press below). Raising re-seats the window among
        // its siblings; the hit widget pointers stay valid.
        if (hit) {
            if (Window* win = windowAncestor(hit)) raiseWindow(win);
        }
        applyFocus((hit && hit->focusable()) ? hit : nullptr);
        if (!hit) return false;
        bool handled = bubblePointer(hit, e);
        // Post-bubble capture revalidation, as in the popup branch
        // above (round-2 review C1).
        if (handled) capture_ = widgetAlive(hit) ? hit : nullptr;
        return handled;
    }
    if (capture_) {
        Widget* target = capture_;
        bool handled = bubblePointer(target, e);
        if (e.type == PointerEvent::Type::Up) capture_ = nullptr;
        return handled;
    }
    if (e.type == PointerEvent::Type::Move) {
        // Hover tracking feeds the WS_HOVERED state bit.
        applyHover(hitTestLogical(e.x, e.y));
    }
    if (Popup* top = topInteractivePopup()) {
        Widget* hit = hitRecurse(top, e.x, e.y);
        if (!hit) return top->flavor() == Popup::Flavor::Modal;
        return bubblePointer(hit, e);
    }
    Widget* hit = hitTestLogical(e.x, e.y);
    if (!hit) return false;
    return bubblePointer(hit, e);
}

bool UiContext::dispatchKey(const KeyEvent& e) {
    if (e.down && e.key == KEY_ESCAPE && !popupStack_.empty()) {
        // Escape closes the top INTERACTIVE popup; overlays above it
        // are dismissed along the way without consuming the key for
        // themselves. If only overlays are open, Escape is not
        // consumed at all (review CORE-M12).
        Popup* top = topInteractivePopup();
        if (!top) return false;
        while (!popupStack_.empty() && popupStack_.back() != top) closeTopPopup();
        closeTopPopup();
        return true;
    }
    // Focus revalidation (review CORE-M8): focus that left its tree,
    // hid, or was disabled is dropped before routing. A focus widget
    // destroyed without a setRoot is dropped by pointer comparison
    // only (round-2 review C1's rule); focusChainValid runs only on
    // a focus proven alive.
    if (focus_ && !widgetAlive(focus_)) focus_ = nullptr;
    if (focus_ && !focusChainValid(focus_)) applyFocus(nullptr);
    Widget* start = focus_ ? focus_ : root_;
    // Modal key capture (review CORE-H4): while the top interactive
    // popup is a modal, keys route into its subtree only.
    if (Popup* top = topInteractivePopup()) {
        if (top->flavor() == Popup::Flavor::Modal &&
            (!start || !isInTree(start, top)))
            start = top;
    }
    // Snapshot walk (round-2 review C2): the key bubble uses the
    // same snapshotted ancestor chain as bubblePointer, so a key
    // handler that destroys its tree and returns false cannot walk
    // the dispatch into a dead parent on the next step.
    std::vector<Widget*> chain = snapshotChain(start);
    for (Widget* w : chain) {
        if (!widgetAlive(w)) continue;
        if (w->handleKey(e)) return true;
    }
    return false;
}

void UiContext::emitDraw(DrawList& dl) const {
    // One call is one frame: the list is cleared at entry (review
    // CORE-M3), so a shell may reuse a DrawList across frames.
    dl.clear();
    DrawParams params{&style_, &metrics(), focus_, style_.uiOpacity};
    // Reap any stale focus/hover bits setRoot recorded (review
    // C4): the trees drawn here are alive by construction, so a
    // stale holder found in them can have its bit safely cleared.
    if (staleFocusBit_ || staleHoverBit_) {
        if (root_) reapStaleBits(root_);
        for (Popup* p : popupStack_) reapStaleBits(p);
    }
    if (root_) root_->draw(dl, params);
    // Popups draw from a SNAPSHOT of the stack (round-2 review
    // S2): a popup's draw() is overridable and may close or destroy
    // popups; each snapshot entry is drawn only while it is still
    // on the live stack (pointer comparison), so a popup destroyed
    // by an earlier popup's draw is skipped, never dereferenced.
    const std::vector<Popup*> stackSnapshot = popupStack_;
    for (Popup* p : stackSnapshot) {
        bool stillOpen = false;
        for (Popup* q : popupStack_)
            if (q == p) { stillOpen = true; break; }
        if (stillOpen) p->draw(dl, params);
    }
    // Glyph expansion: with a glyph source bound, fully covered
    // TextRuns become GlyphQuads here, in logical units, before the
    // output transform (GlyphQuad destination rects scale like every
    // rect; their UVs are normalized and do not).
    if (glyphSource_) expandGlyphRuns(dl, *glyphSource_, metrics());
    // Output transform: the tree drew in logical units; the list that
    // leaves the boundary is physical. Skipped at exactly 1.0, so the
    // unscaled path is byte-identical to before uiScale existed. This
    // is the ONLY place draw output is scaled: every emission path
    // (cLithoGui::emitFrame included) funnels through here.
    const float s = scaleFactor();
    if (s != 1.0f) dl.scale(s);
}

// --------------------------------------------------------- UiStateMachine

UiStateMachine::UiStateMachine() {
    initRoot_ = buildInitTree();
    menuRoot_ = buildMenuTree();
    ctx_.setRoot(initRoot_.get());
}

Widget* UiStateMachine::rootFor(UiState s) {
    return s == UiState::InitWindow ? initRoot_.get() : menuRoot_.get();
}

void UiStateMachine::switchTo(UiState s) {
    state_ = s;
    ctx_.setRoot(rootFor(s));
}

std::unique_ptr<Widget> UiStateMachine::buildInitTree() {
    auto win = std::make_unique<Window>("init-window", "LithoLattice");
    win->setFrame({120.0f, 90.0f, 420.0f, 240.0f});
    auto panel = std::make_unique<Panel>("init-panel");
    panel->dir = LayoutDir::Column;
    panel->padding = Edges(16.0f);
    panel->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
    auto status = std::make_unique<Label>("init-status", "Initializing engine...");
    status->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(status));
    auto field = std::make_unique<TextField>("init-note");
    field->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(field));
    auto btn = std::make_unique<Button>("init-continue", "Continue");
    btn->fixedH = 32.0f;
    btn->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(btn));
    win->addChild(std::move(panel));
    return win;
}

std::unique_ptr<Widget> UiStateMachine::buildMenuTree() {
    // Transparent root: the world renders behind the main menu in the shell.
    auto root = std::make_unique<Panel>("menu-root");
    root->setBackground(Color{0, 0, 0, 0});
    root->dir = LayoutDir::None;
    auto win = std::make_unique<Window>("menu-window", "LithoKraft");
    win->setFrame({40.0f, 40.0f, 300.0f, 340.0f});
    auto panel = std::make_unique<Panel>("menu-panel");
    panel->dir = LayoutDir::Column;
    panel->padding = Edges(14.0f);
    panel->anchors = ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP | ANCHOR_BOTTOM;
    auto title = std::make_unique<Label>("menu-title", "Main Menu");
    title->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(title));
    auto play = std::make_unique<Button>("menu-play", "Play");
    play->fixedH = 36.0f;
    play->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(play));
    auto settings = std::make_unique<Button>("menu-settings", "Settings");
    settings->fixedH = 36.0f;
    settings->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(settings));
    auto fs = std::make_unique<Toggle>("menu-fullscreen", "Fullscreen", false);
    fs->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(fs));
    auto worlds = std::make_unique<ListBox>("menu-worlds");
    worlds->items() = {"Wafer - Creative", "Wafer - Survival", "Test World"};
    worlds->fixedH = 66.0f;
    worlds->margin = Edges(0.0f, 6.0f);
    panel->addChild(std::move(worlds));
    win->addChild(std::move(panel));
    root->addChild(std::move(win));
    return root;
}

// ------------------------------------------------------------- theme load

namespace {

void themeErr(std::vector<std::string>& errors, const std::string& ctxName, const std::string& what) {
    errors.push_back(ctxName + ": " + what + " (kept default)");
}

bool themeNumber(const JsonValue& obj, const char* key, float& out,
                 std::vector<std::string>& errors, const char* ctxName) {
    const JsonValue* v = obj.find(key);
    if (!v) return false;
    if (!v->isNumber()) {
        themeErr(errors, std::string(ctxName) + "." + key, "expected a number");
        return false;
    }
    // Numeric hardening (review SUP-1): a non-finite value, or one
    // outside float range, is rejected with an error and the current
    // value is kept. The old bare static_cast turned 1e20 into a
    // valid-looking float and huge values into inf/UB downstream.
    // The bound is numeric_limits<float>::max() EXACTLY, with an
    // isfinite check after the cast (round-2 review SUP2-4): the
    // earlier decimal bound sat ABOVE FLT_MAX, so a boundary value
    // passed the check and the cast wrote inf into the live Style.
    const double d = v->number;
    const double fmax = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(d) || d > fmax || d < -fmax) {
        themeErr(errors, std::string(ctxName) + "." + key, "number is not a finite float");
        return false;
    }
    out = static_cast<float>(d);
    if (!std::isfinite(out)) {
        themeErr(errors, std::string(ctxName) + "." + key, "number is not a finite float");
        return false;
    }
    return true;
}

// An integer token (font id, weight): must be a finite number inside
// int range with NO fractional part, else an error and the current
// value is kept (SUP-1; fractions are rejected, not truncated,
// round-2 review SUP2-8).
bool themeIntValue(const JsonValue& v, int& out, std::vector<std::string>& errors,
                   const char* ctxName) {
    if (!v.isNumber() || !std::isfinite(v.number) ||
        v.number < -2147483648.0 || v.number > 2147483647.0 ||
        v.number != std::trunc(v.number)) {
        themeErr(errors, ctxName, "expected an integer in int range");
        return false;
    }
    out = static_cast<int>(v.number);
    return true;
}

bool themeColorValue(const JsonValue& v, Color& out, std::vector<std::string>& errors,
                     const char* ctxName) {
    if (!v.isArray() || (v.array.size() != 3 && v.array.size() != 4)) {
        themeErr(errors, ctxName, "expected a [r,g,b] or [r,g,b,a] array");
        return false;
    }
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (std::size_t i = 0; i < v.array.size(); ++i) {
        if (!v.array[i].isNumber()) {
            themeErr(errors, ctxName, "color components must be numbers");
            return false;
        }
        // Color policy (review SUP-1, chosen and documented):
        // components outside [0,1] or non-finite REJECT the whole
        // color with an error; the current color is kept. (The
        // alternative, clamping, would silently re-tint a theme.)
        const double d = v.array[i].number;
        if (!std::isfinite(d) || d < 0.0 || d > 1.0) {
            themeErr(errors, ctxName, "color component outside [0,1]");
            return false;
        }
        rgba[i] = static_cast<float>(d);
    }
    out = {rgba[0], rgba[1], rgba[2], rgba[3]};
    return true;
}

void themeTextStyleValue(const JsonValue& v, TextStyle& ts, std::vector<std::string>& errors,
                         const char* ctxName) {
    if (!v.isObject()) {
        themeErr(errors, ctxName, "expected an object");
        return;
    }
    if (const JsonValue* f = v.find("font")) {
        int id = 0;
        if (themeIntValue(*f, id, errors, (std::string(ctxName) + ".font").c_str()))
            ts.font = static_cast<FontId>(id);
    }
    themeNumber(v, "size", ts.size, errors, ctxName);
    themeNumber(v, "letterSpacing", ts.letterSpacing, errors, ctxName);
    themeNumber(v, "lineHeightMul", ts.lineHeightMul, errors, ctxName);
    if (const JsonValue* w = v.find("weight")) {
        int weight = 0;
        if (themeIntValue(*w, weight, errors, (std::string(ctxName) + ".weight").c_str()))
            ts.weight = weight;
    }
    if (const JsonValue* s = v.find("slant")) {
        if (!s->isString()) {
            themeErr(errors, std::string(ctxName) + ".slant", "expected a string");
        } else if (s->str == "normal") ts.slant = FontSlant::Normal;
        else if (s->str == "italic") ts.slant = FontSlant::Italic;
        else if (s->str == "oblique") ts.slant = FontSlant::Oblique;
        else themeErr(errors, std::string(ctxName) + ".slant", "unknown slant '" + s->str + "'");
    }
    if (const JsonValue* c = v.find("color")) {
        Color col;
        if (themeColorValue(*c, col, errors, (std::string(ctxName) + ".color").c_str()))
            ts.color = col;
    }
    if (const JsonValue* fb = v.find("fallbacks")) {
        if (fb->isArray()) {
            std::vector<FontId> chain;
            bool okIds = true;
            for (auto& e : fb->array) {
                int id = 0;
                std::vector<std::string> scratch; // range errors reported below, once
                if (themeIntValue(e, id, scratch, "fallback")) chain.push_back(static_cast<FontId>(id));
                else okIds = false;
            }
            if (okIds) ts.fallbacks = std::move(chain);
            else themeErr(errors, std::string(ctxName) + ".fallbacks", "expected font ids in int range");
        } else {
            themeErr(errors, std::string(ctxName) + ".fallbacks", "expected an array");
        }
    }
}

} // namespace

bool applyThemeJson(Style& style, ThemeShaders& shaders, const std::string& jsonText,
                    std::vector<std::string>& errors) {
    JsonValue root;
    std::string parseError;
    if (!parseJson(jsonText, root, parseError) || !root.isObject()) {
        if (parseError.empty()) parseError = "theme root is not a JSON object";
        style = Style{};
        shaders.clear();
        errors.push_back("theme: " + parseError + " (defaults restored)");
        return false;
    }

    if (const JsonValue* pal = root.find("palette")) {
        if (!pal->isObject()) {
            themeErr(errors, "palette", "expected an object");
        } else {
            struct Entry { const char* key; Color Style::* field; };
            static const Entry entries[] = {
                {"windowBg", &Style::windowBg},   {"titleBar", &Style::titleBar},
                {"panelBg", &Style::panelBg},     {"controlBg", &Style::controlBg},
                {"controlBgHot", &Style::controlBgHot}, {"selection", &Style::selection},
                {"text", &Style::text},           {"textMuted", &Style::textMuted},
                {"border", &Style::border},       {"traceRed", &Style::traceRed},
                {"traceBlue", &Style::traceBlue}, {"tracePurple", &Style::tracePurple},
            };
            for (const Entry& e : entries) {
                if (const JsonValue* v = pal->find(e.key)) {
                    Color col;
                    std::string ctxName = std::string("palette.") + e.key;
                    if (themeColorValue(*v, col, errors, ctxName.c_str())) style.*(e.field) = col;
                }
            }
        }
    }

    if (const JsonValue* op = root.find("opacity")) {
        if (!op->isObject()) themeErr(errors, "opacity", "expected an object");
        else {
            themeNumber(*op, "global", style.uiOpacity, errors, "opacity");
            themeNumber(*op, "window", style.windowOpacity, errors, "opacity");
            themeNumber(*op, "trace", style.traceOpacity, errors, "opacity");
        }
    }

    themeNumber(root, "borderWidth", style.borderWidth, errors, "theme");
    themeNumber(root, "uiScale", style.uiScale, errors, "theme");
    themeNumber(root, "cornerRadius", style.cornerRadius, errors, "theme");
    themeNumber(root, "titleBarHeight", style.titleBarHeight, errors, "theme");
    themeNumber(root, "rowHeight", style.rowHeight, errors, "theme");

    if (const JsonValue* sp = root.find("spacing")) {
        if (!sp->isObject()) themeErr(errors, "spacing", "expected an object");
        else themeNumber(*sp, "defaultPadding", style.defaultPadding, errors, "spacing");
    }

    if (const JsonValue* tss = root.find("textStyles")) {
        if (!tss->isObject()) themeErr(errors, "textStyles", "expected an object");
        else {
            if (const JsonValue* b = tss->find("body"))
                themeTextStyleValue(*b, style.bodyText, errors, "textStyles.body");
            if (const JsonValue* t = tss->find("title"))
                themeTextStyleValue(*t, style.titleText, errors, "textStyles.title");
        }
    }

    if (const JsonValue* tr = root.find("trace")) {
        if (!tr->isObject()) themeErr(errors, "trace", "expected an object");
        else {
            themeNumber(*tr, "defaultPhase", style.tracePhase, errors, "trace");
            themeNumber(*tr, "phaseSpeed", style.tracePhaseSpeed, errors, "trace");
        }
    }

    if (const JsonValue* sh = root.find("shaders")) {
        if (!sh->isObject()) themeErr(errors, "shaders", "expected an object");
        else {
            for (auto& kv : sh->object) {
                if (kv.second.isString()) shaders.set(kv.first, kv.second.str);
                else themeErr(errors, "shaders." + kv.first, "expected a file path string");
            }
        }
    }

    // THE numeric validation table (review SUP-1 / CORE-M4, unified
    // in round 2 as SUP2-5): EVERY numeric token a theme can set is
    // validated here, at apply time, under one consistent policy:
    //   - metrics and sizes: CLAMP to the documented range and
    //     report an error entry (a non-finite value reports and
    //     falls back to the default instead);
    //   - colors: REJECT the whole color and keep the current one
    //     (themeColorValue above; clamping would silently re-tint);
    //   - opacities: clamp to [0, 1] and report, like metrics.
    // Unbounded-but-finite by design (dimensionless or signed
    // quantities with guarded consumers): tracePhase, tracePhaseSpeed,
    // letterSpacing. Whatever the tokens (or the incoming style)
    // held, the applied style is livable: never UB, never a dead
    // list, never an inf in a live Style.
    auto clampMetric = [&errors](float v, float lo, float hi, float fallback,
                                 const char* name) {
        if (!std::isfinite(v)) {
            errors.push_back(std::string(name) + ": metric is not finite (kept default)");
            return fallback;
        }
        if (v < lo) {
            errors.push_back(std::string(name) + ": metric below its range (clamped)");
            return lo;
        }
        if (v > hi) {
            errors.push_back(std::string(name) + ": metric above its range (clamped)");
            return hi;
        }
        return v;
    };
    style.rowHeight = clampMetric(style.rowHeight, 4.0f, 4096.0f, 22.0f, "theme.rowHeight");
    style.titleBarHeight =
        clampMetric(style.titleBarHeight, 4.0f, 4096.0f, 28.0f, "theme.titleBarHeight");
    style.cornerRadius =
        clampMetric(style.cornerRadius, 0.0f, 4096.0f, 4.0f, "theme.cornerRadius");
    style.borderWidth =
        clampMetric(style.borderWidth, 0.0f, 1024.0f, 1.0f, "theme.borderWidth");
    style.defaultPadding =
        clampMetric(style.defaultPadding, 0.0f, 4096.0f, 8.0f, "theme.defaultPadding");
    style.uiScale = clampMetric(style.uiScale, 0.1f, 8.0f, 1.0f, "theme.uiScale");
    // lineHeightMul (round-2 review SUP2-1): the one metric SUP-1
    // missed; 0 / negative / tiny values reached a float->int cast
    // in TextField draw and aborted under float-cast-overflow.
    style.bodyText.lineHeightMul = clampMetric(style.bodyText.lineHeightMul, 0.5f, 4.0f,
                                               1.2f, "textStyles.body.lineHeightMul");
    style.titleText.lineHeightMul = clampMetric(style.titleText.lineHeightMul, 0.5f, 4.0f,
                                                1.2f, "textStyles.title.lineHeightMul");
    auto clampSize = [&errors](float v, const char* name) {
        if (!std::isfinite(v) || v <= 0.0f) {
            errors.push_back(std::string(name) + ": text size outside (0, 256] (kept default)");
            return 14.0f;
        }
        if (v > 256.0f) {
            errors.push_back(std::string(name) + ": text size outside (0, 256] (clamped)");
            return 256.0f;
        }
        return v;
    };
    style.bodyText.size = clampSize(style.bodyText.size, "textStyles.body.size");
    style.titleText.size = clampSize(style.titleText.size, "textStyles.title.size");
    auto clampOpacity = [&errors](float v, const char* name) {
        if (!std::isfinite(v)) {
            errors.push_back(std::string(name) + ": opacity is not finite (kept default)");
            return 1.0f;
        }
        if (v < 0.0f || v > 1.0f) {
            errors.push_back(std::string(name) + ": opacity outside [0, 1] (clamped)");
            return clampf(v, 0.0f, 1.0f);
        }
        return v;
    };
    style.uiOpacity = clampOpacity(style.uiOpacity, "opacity.global");
    style.windowOpacity = clampOpacity(style.windowOpacity, "opacity.window");
    style.traceOpacity = clampOpacity(style.traceOpacity, "opacity.trace");
    return true;
}

// --------------------------------------------------------------- cLithoGui

bool cLithoGui::applyThemeText(const std::string& json, std::vector<std::string>* errors) {
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;
    // Overlay onto the CURRENT context style (review CORE-M2):
    // tokens the theme does not name keep the values the shell set,
    // so a theme load no longer resets uiScale / opacities the shell
    // bound. (On a document-level parse failure applyThemeJson still
    // restores full defaults, as documented on the free function.)
    Style s = ctx_.style();
    ThemeShaders sh = shaders_;
    bool ok = lithogui::applyThemeJson(s, sh, json, errs);
    ctx_.setStyle(s);
    shaders_ = sh;
    return ok;
}

bool cLithoGui::applyThemeJson(const std::string& json, std::vector<std::string>* errors) {
    return applyThemeText(json, errors);
}

bool cLithoGui::loadThemeFromFile(const std::string& path, std::vector<std::string>* errors) {
    std::ifstream in(path);
    if (!in) {
        if (errors) errors->push_back("theme: cannot open file '" + path + "'");
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    // Verify the read actually succeeded (review CORE-L7): a
    // directory or an unreadable file reports an error here and the
    // current theme is left untouched, instead of an empty/garbled
    // buffer being parsed as a theme.
    if (in.bad() || ss.fail()) {
        if (errors) errors->push_back("theme: failed reading file '" + path + "'");
        return false;
    }
    return applyThemeText(ss.str(), errors);
}

void cLithoGui::emitFrame(DrawList& dl, float tracePhase) {
    Style s = ctx_.style();
    s.tracePhase = tracePhase;
    ctx_.setStyle(s);
    dl.clear();
    ctx_.emitDraw(dl);
}

} // namespace lithogui
