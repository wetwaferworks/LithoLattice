// Litho GUI reference font layer - implementation. See litho_font.h.
//
// The Latin face is a single-stroke geometric sans, authored here as
// polylines in a 100-unit em: x right, y UP, baseline at y = 0, cap
// height 70, x-height 52, descenders to -20. A point of {-1,-1} in a
// glyph's table lifts the pen (starts a new stroke). The design is
// deliberately plain: consistent stroke, squared terminals, lozenge
// bowls. It is an original in-house design, not traced from any font.
#include "litholattice/ui/litho_font.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lithogui {
namespace {

// ------------------------------------------------------------- glyph data
struct StrokePt {
    std::int8_t x = 0;
    std::int8_t y = 0;
};
constexpr StrokePt UP{-1, -1}; // pen lift between strokes

struct GlyphDef {
    char32_t cp;
    int adv; // advance in font units
    const StrokePt* pts;
    int count;
};

#define LG(cp_, adv_, arr_) \
    GlyphDef { cp_, adv_, arr_, static_cast<int>(sizeof(arr_) / sizeof((arr_)[0])) }

// Punctuation and symbols.
const StrokePt pBang[] = {{14, 70}, {14, 18}, UP, {14, 0}, {14, 5}};
const StrokePt pDQuote[] = {{8, 70}, {8, 56}, UP, {24, 70}, {24, 56}};
const StrokePt pHash[] = {{14, 0}, {20, 70}, UP, {42, 0}, {48, 70}, UP,
                          {4, 24}, {58, 24}, UP, {4, 46}, {58, 46}};
const StrokePt pDollar[] = {{44, 58}, {32, 68}, {16, 64}, {10, 50}, {18, 38},
                            {38, 28}, {44, 14}, {34, 2}, {16, 2}, {6, 12}, UP,
                            {28, 78}, {28, -8}};
const StrokePt pPercent[] = {{4, 52}, {18, 52}, {18, 66}, {4, 66}, {4, 52}, UP,
                             {64, 4}, {20, 66}, UP,
                             {66, 4}, {80, 4}, {80, 18}, {66, 18}, {66, 4}};
const StrokePt pAmp[] = {{44, 66}, {26, 68}, {14, 56}, {22, 42}, {42, 22},
                         {50, 8}, {40, 0}, {24, 6}, {12, 20}, {10, 36},
                         {20, 50}, UP, {30, 30}, {52, 62}};
const StrokePt pSQuote[] = {{10, 70}, {10, 56}};
const StrokePt pLParen[] = {{24, 76}, {12, 54}, {7, 35}, {12, 16}, {24, -6}};
const StrokePt pRParen[] = {{8, 76}, {20, 54}, {25, 35}, {20, 16}, {8, -6}};
const StrokePt pStar[] = {{22, 38}, {22, 66}, UP, {10, 44}, {34, 60}, UP,
                          {10, 60}, {34, 44}};
const StrokePt pPlus[] = {{26, 12}, {26, 58}, UP, {6, 35}, {46, 35}};
const StrokePt pComma[] = {{16, 2}, {16, 8}, UP, {16, 2}, {9, -8}};
const StrokePt pMinus[] = {{6, 32}, {34, 32}};
const StrokePt pDot[] = {{14, 0}, {14, 5}};
const StrokePt pSlash[] = {{34, 0}, {6, 70}};
const StrokePt pColon[] = {{14, 44}, {14, 49}, UP, {14, 12}, {14, 17}};
const StrokePt pSemi[] = {{14, 44}, {14, 49}, UP, {15, 14}, {8, 2}};
const StrokePt pLt[] = {{44, 58}, {10, 32}, {44, 6}};
const StrokePt pEq[] = {{6, 42}, {42, 42}, UP, {6, 22}, {42, 22}};
const StrokePt pGt[] = {{8, 58}, {42, 32}, {8, 6}};
const StrokePt pQuest[] = {{10, 56}, {18, 68}, {34, 68}, {42, 58}, {40, 44},
                           {26, 32}, {26, 20}, UP, {26, 0}, {26, 5}};
const StrokePt pAt[] = {{56, 50}, {44, 62}, {26, 64}, {12, 50}, {8, 28},
                        {16, 6}, {36, 0}, {58, 6}, {68, 20}, UP,
                        {32, 18}, {32, 42}, {50, 42}, {50, 18}, {32, 18}};
const StrokePt pLBrack[] = {{24, 76}, {10, 76}, {10, -6}, {24, -6}};
const StrokePt pBSlash[] = {{6, 70}, {34, 0}};
const StrokePt pRBrack[] = {{8, 76}, {22, 76}, {22, -6}, {8, -6}};
const StrokePt pCaret[] = {{8, 44}, {22, 62}, {36, 44}};
const StrokePt pUnder[] = {{4, -14}, {44, -14}};
const StrokePt pGrave[] = {{8, 70}, {16, 56}};
const StrokePt pLBrace[] = {{26, 76}, {17, 69}, {15, 53}, {15, 43}, {7, 36},
                            {15, 29}, {15, 15}, {19, 2}, {26, -6}};
const StrokePt pBar[] = {{12, -6}, {12, 76}};
const StrokePt pRBrace[] = {{10, 76}, {19, 69}, {21, 53}, {21, 43}, {29, 36},
                            {21, 29}, {21, 15}, {17, 2}, {10, -6}};
const StrokePt pTilde[] = {{6, 30}, {16, 40}, {26, 30}, {36, 22}, {44, 30}};

// Digits (advance 56).
const StrokePt pD0[] = {{14, 0}, {8, 22}, {8, 48}, {14, 70}, {42, 70},
                        {48, 48}, {48, 22}, {42, 0}, {14, 0}};
const StrokePt pD1[] = {{18, 56}, {30, 70}, {30, 0}, UP, {14, 0}, {46, 0}};
const StrokePt pD2[] = {{8, 52}, {14, 66}, {28, 70}, {42, 66}, {48, 52},
                        {48, 40}, {8, 0}, {48, 0}};
const StrokePt pD3[] = {{10, 64}, {28, 70}, {42, 62}, {40, 46}, {28, 36},
                        {40, 30}, {42, 14}, {28, 0}, {10, 4}};
const StrokePt pD4[] = {{36, 0}, {36, 70}, UP, {36, 70}, {8, 18}, UP,
                        {2, 18}, {52, 18}};
const StrokePt pD5[] = {{46, 70}, {12, 70}, {10, 40}, {28, 44}, {42, 38},
                        {46, 22}, {40, 6}, {24, 0}, {10, 4}};
const StrokePt pD6[] = {{42, 64}, {26, 68}, {12, 52}, {8, 28}, {12, 8},
                        {26, 0}, {40, 2}, {48, 14}, {44, 30}, {30, 38}, {14, 30}};
const StrokePt pD7[] = {{8, 70}, {48, 70}, {30, 0}};
const StrokePt pD8[] = {{28, 70}, {12, 62}, {12, 48}, {28, 36}, {44, 48},
                        {44, 62}, {28, 70}, UP,
                        {28, 36}, {10, 26}, {12, 8}, {28, 0}, {44, 8}, {46, 26},
                        {28, 36}};
const StrokePt pD9[] = {{14, 6}, {30, 0}, {44, 14}, {48, 42}, {44, 62},
                        {30, 70}, {16, 68}, {8, 56}, {12, 40}, {26, 32}, {44, 40}};

// Uppercase (advance 58 unless noted).
const StrokePt pUA[] = {{2, 0}, {29, 70}, {56, 0}, UP, {14, 26}, {44, 26}};
const StrokePt pUB[] = {{10, 0}, {10, 70}, UP, {10, 70}, {34, 70}, {44, 62},
                        {42, 47}, {32, 38}, {10, 38}, UP,
                        {10, 38}, {36, 38}, {46, 28}, {44, 10}, {32, 0}, {10, 0}};
const StrokePt pUC[] = {{50, 58}, {38, 68}, {20, 68}, {8, 54}, {6, 36},
                        {8, 16}, {20, 2}, {38, 2}, {50, 12}};
const StrokePt pUD[] = {{10, 0}, {10, 70}, UP, {10, 70}, {34, 70}, {48, 52},
                        {48, 18}, {34, 0}, {10, 0}};
const StrokePt pUE[] = {{10, 0}, {10, 70}, UP, {10, 70}, {46, 70}, UP,
                        {10, 36}, {38, 36}, UP, {10, 0}, {46, 0}};
const StrokePt pUF[] = {{10, 0}, {10, 70}, UP, {10, 70}, {46, 70}, UP,
                        {10, 36}, {36, 36}};
const StrokePt pUG[] = {{50, 58}, {38, 68}, {20, 68}, {8, 54}, {6, 36},
                        {8, 16}, {20, 2}, {38, 2}, {50, 10}, {50, 30}, {34, 30}};
const StrokePt pUH[] = {{8, 0}, {8, 70}, UP, {48, 0}, {48, 70}, UP,
                        {8, 36}, {48, 36}};
const StrokePt pUI[] = {{15, 0}, {15, 70}, UP, {4, 70}, {26, 70}, UP,
                        {4, 0}, {26, 0}};
const StrokePt pUJ[] = {{24, 70}, {38, 70}, UP, {38, 70}, {38, 16}, {30, 2},
                        {16, 0}, {6, 10}};
const StrokePt pUK[] = {{10, 0}, {10, 70}, UP, {46, 70}, {12, 32}, UP,
                        {18, 40}, {48, 0}};
const StrokePt pUL[] = {{10, 0}, {10, 70}, UP, {10, 0}, {46, 0}};
const StrokePt pUM[] = {{6, 0}, {6, 70}, {39, 26}, {72, 70}, {72, 0}};
const StrokePt pUN[] = {{8, 0}, {8, 70}, {50, 0}, {50, 70}};
const StrokePt pUO[] = {{18, 0}, {8, 18}, {8, 52}, {18, 70}, {44, 70},
                        {54, 52}, {54, 18}, {44, 0}, {18, 0}};
const StrokePt pUP[] = {{10, 0}, {10, 70}, UP, {10, 70}, {38, 70}, {48, 58},
                        {46, 42}, {34, 34}, {10, 34}};
const StrokePt pUQ[] = {{18, 0}, {8, 18}, {8, 52}, {18, 70}, {44, 70},
                        {54, 52}, {54, 18}, {44, 0}, {18, 0}, UP,
                        {36, 16}, {54, -6}};
const StrokePt pUR[] = {{10, 0}, {10, 70}, UP, {10, 70}, {38, 70}, {48, 58},
                        {46, 42}, {34, 34}, {10, 34}, UP, {32, 34}, {50, 0}};
const StrokePt pUS[] = {{48, 60}, {34, 70}, {16, 66}, {8, 54}, {14, 40},
                        {44, 26}, {48, 12}, {36, 0}, {16, 0}, {6, 8}};
const StrokePt pUT[] = {{28, 0}, {28, 70}, UP, {2, 70}, {54, 70}};
const StrokePt pUU[] = {{8, 70}, {8, 16}, {16, 2}, {40, 2}, {48, 16}, {48, 70}};
const StrokePt pUV[] = {{4, 70}, {28, 0}, {52, 70}};
const StrokePt pUW[] = {{4, 70}, {14, 0}, {40, 44}, {66, 0}, {76, 70}};
const StrokePt pUX[] = {{8, 0}, {50, 70}, UP, {8, 70}, {50, 0}};
const StrokePt pUY[] = {{4, 70}, {29, 38}, {54, 70}, UP, {29, 38}, {29, 0}};
const StrokePt pUZ[] = {{8, 70}, {50, 70}, {8, 0}, {50, 0}};

// Lowercase (x-height 52).
const StrokePt pLa[] = {{44, 26}, {36, 48}, {18, 52}, {8, 40}, {8, 12},
                        {18, 0}, {36, 0}, {44, 12}, {44, 26}, UP,
                        {44, 52}, {44, 0}};
const StrokePt pLb[] = {{10, 0}, {10, 70}, UP, {10, 34}, {26, 52}, {42, 48},
                        {46, 30}, {40, 8}, {24, 0}, {10, 6}};
const StrokePt pLc[] = {{44, 42}, {32, 52}, {16, 50}, {7, 38}, {7, 14},
                        {16, 2}, {32, 0}, {44, 10}};
const StrokePt pLd[] = {{46, 0}, {46, 70}, UP, {46, 34}, {30, 52}, {14, 48},
                        {8, 28}, {16, 6}, {32, 0}, {46, 6}};
const StrokePt pLe[] = {{8, 26}, {46, 26}, UP, {46, 26}, {42, 44}, {26, 52},
                        {12, 46}, {8, 26}, {10, 10}, {24, 0}, {38, 2}, {46, 12}};
const StrokePt pLf[] = {{30, 70}, {20, 66}, {16, 52}, {16, 0}, UP,
                        {6, 38}, {28, 38}};
const StrokePt pLg[] = {{44, 26}, {36, 48}, {18, 52}, {8, 40}, {8, 12},
                        {18, 0}, {36, 0}, {44, 12}, {44, 26}, UP,
                        {44, 52}, {44, 8}, {38, -12}, {20, -18}, {8, -10}};
const StrokePt pLh[] = {{10, 0}, {10, 70}, UP, {10, 36}, {20, 50}, {36, 52},
                        {46, 40}, {46, 0}};
const StrokePt pLi[] = {{13, 0}, {13, 52}, UP, {13, 64}, {13, 69}};
const StrokePt pLj[] = {{22, 52}, {22, 8}, {16, -12}, {4, -16}, UP,
                        {22, 64}, {22, 69}};
const StrokePt pLk[] = {{8, 0}, {8, 70}, UP, {40, 52}, {10, 24}, UP,
                        {16, 30}, {42, 0}};
const StrokePt pLl[] = {{13, 0}, {13, 70}};
const StrokePt pLm[] = {{6, 0}, {6, 52}, UP, {6, 38}, {14, 50}, {28, 52},
                        {36, 42}, {36, 0}, UP, {36, 38}, {44, 50}, {58, 52},
                        {68, 42}, {68, 0}};
const StrokePt pLn[] = {{8, 0}, {8, 52}, UP, {8, 38}, {18, 50}, {34, 52},
                        {46, 40}, {46, 0}};
const StrokePt pLo[] = {{28, 0}, {12, 6}, {6, 26}, {12, 46}, {28, 52},
                        {44, 46}, {50, 26}, {44, 6}, {28, 0}};
const StrokePt pLp[] = {{10, -20}, {10, 52}, UP, {10, 34}, {26, 52}, {42, 48},
                        {46, 30}, {40, 8}, {24, 0}, {10, 6}};
const StrokePt pLq[] = {{46, -20}, {46, 52}, UP, {46, 34}, {30, 52}, {14, 48},
                        {8, 28}, {16, 6}, {32, 0}, {46, 6}};
const StrokePt pLr[] = {{8, 0}, {8, 52}, UP, {8, 36}, {18, 50}, {32, 52}, {38, 46}};
const StrokePt pLs[] = {{42, 44}, {30, 52}, {14, 50}, {8, 40}, {16, 28},
                        {38, 20}, {42, 8}, {30, 0}, {12, 0}, {6, 6}};
const StrokePt pLowT[] = {{16, 70}, {16, 10}, {22, 0}, {32, 0}, UP,
                        {6, 38}, {28, 38}};
const StrokePt pLu[] = {{8, 52}, {8, 14}, {16, 2}, {32, 0}, {46, 10}, {46, 52},
                        UP, {46, 52}, {46, 0}};
const StrokePt pLv[] = {{4, 52}, {26, 0}, {48, 52}};
const StrokePt pLw[] = {{4, 52}, {12, 0}, {37, 34}, {62, 0}, {70, 52}};
const StrokePt pLx[] = {{8, 0}, {40, 52}, UP, {8, 52}, {40, 0}};
const StrokePt pLy[] = {{6, 52}, {24, 14}, UP, {42, 52}, {24, 14}, {14, -16}};
const StrokePt pLz[] = {{8, 52}, {40, 52}, {8, 0}, {40, 0}};

const GlyphDef kLatinGlyphs[] = {
    GlyphDef{U' ', 30, nullptr, 0},
    LG(U'!', 28, pBang), LG(U'"', 32, pDQuote), LG(U'#', 62, pHash),
    LG(U'$', 56, pDollar), LG(U'%', 84, pPercent), LG(U'&', 62, pAmp),
    LG(U'\'', 20, pSQuote), LG(U'(', 32, pLParen), LG(U')', 32, pRParen),
    LG(U'*', 44, pStar), LG(U'+', 52, pPlus), LG(U',', 28, pComma),
    LG(U'-', 40, pMinus), LG(U'.', 28, pDot), LG(U'/', 40, pSlash),
    LG(U'0', 56, pD0), LG(U'1', 56, pD1), LG(U'2', 56, pD2), LG(U'3', 56, pD3),
    LG(U'4', 56, pD4), LG(U'5', 56, pD5), LG(U'6', 56, pD6), LG(U'7', 56, pD7),
    LG(U'8', 56, pD8), LG(U'9', 56, pD9),
    LG(U':', 28, pColon), LG(U';', 28, pSemi), LG(U'<', 52, pLt),
    LG(U'=', 48, pEq), LG(U'>', 52, pGt), LG(U'?', 52, pQuest),
    LG(U'@', 84, pAt),
    LG(U'A', 58, pUA), LG(U'B', 58, pUB), LG(U'C', 58, pUC), LG(U'D', 58, pUD),
    LG(U'E', 58, pUE), LG(U'F', 58, pUF), LG(U'G', 58, pUG), LG(U'H', 58, pUH),
    LG(U'I', 30, pUI), LG(U'J', 44, pUJ), LG(U'K', 58, pUK), LG(U'L', 58, pUL),
    LG(U'M', 78, pUM), LG(U'N', 58, pUN), LG(U'O', 62, pUO), LG(U'P', 58, pUP),
    LG(U'Q', 62, pUQ), LG(U'R', 58, pUR), LG(U'S', 58, pUS), LG(U'T', 56, pUT),
    LG(U'U', 58, pUU), LG(U'V', 56, pUV), LG(U'W', 80, pUW), LG(U'X', 58, pUX),
    LG(U'Y', 58, pUY), LG(U'Z', 58, pUZ),
    LG(U'[', 32, pLBrack), LG(U'\\', 40, pBSlash), LG(U']', 32, pRBrack),
    LG(U'^', 44, pCaret), LG(U'_', 48, pUnder), LG(U'`', 24, pGrave),
    LG(U'a', 54, pLa), LG(U'b', 54, pLb), LG(U'c', 50, pLc), LG(U'd', 54, pLd),
    LG(U'e', 54, pLe), LG(U'f', 34, pLf), LG(U'g', 54, pLg), LG(U'h', 56, pLh),
    LG(U'i', 26, pLi), LG(U'j', 30, pLj), LG(U'k', 50, pLk), LG(U'l', 26, pLl),
    LG(U'm', 76, pLm), LG(U'n', 56, pLn), LG(U'o', 56, pLo), LG(U'p', 54, pLp),
    LG(U'q', 54, pLq), LG(U'r', 38, pLr), LG(U's', 48, pLs), LG(U't', 34, pLowT),
    LG(U'u', 56, pLu), LG(U'v', 52, pLv), LG(U'w', 74, pLw), LG(U'x', 48, pLx),
    LG(U'y', 50, pLy), LG(U'z', 48, pLz),
    LG(U'{', 36, pLBrace), LG(U'|', 24, pBar), LG(U'}', 36, pRBrace),
    LG(U'~', 48, pTilde),
};
#undef LG

const GlyphDef* latinGlyph(char32_t cp) {
    for (const GlyphDef& g : kLatinGlyphs) {
        if (g.cp == cp) return &g;
    }
    return nullptr;
}

// ------------------------------------------------------------- segments
using Seg = std::pair<Vec2, Vec2>;

void latinSegments(char32_t cp, std::vector<Seg>& out) {
    const GlyphDef* g = latinGlyph(cp);
    if (!g) return;
    Vec2 prev{0, 0};
    bool pen = false;
    for (int i = 0; i < g->count; ++i) {
        const StrokePt& p = g->pts[i];
        if (p.x == -1 && p.y == -1) { pen = false; continue; }
        Vec2 cur{static_cast<float>(p.x), static_cast<float>(p.y)};
        if (pen) out.push_back({prev, cur});
        prev = cur;
        pen = true;
    }
}

// Systerian (Cistercian) generation. One digit's strokes in its
// quadrant-local frame: u runs 0..26 AWAY from the stave, v runs
// 0..48 DOWN from the quadrant's top. The classic digit shapes:
//   1 top bar        2 bottom bar      3 falling diagonal
//   4 rising diagonal  5 = 1+4         6 outer vertical
//   7 = 1+6          8 = 2+6           9 = 1+2+6
void systDigitSegs(int d, std::vector<Seg>& local) {
    auto seg = [&](float u1, float v1, float u2, float v2) {
        local.push_back({{u1, v1}, {u2, v2}});
    };
    switch (d) {
        case 1: seg(0, 0, 26, 0); break;
        case 2: seg(0, 48, 26, 48); break;
        case 3: seg(0, 0, 26, 48); break;
        case 4: seg(0, 48, 26, 0); break;
        case 5: seg(0, 0, 26, 0); seg(0, 48, 26, 0); break;
        case 6: seg(26, 0, 26, 48); break;
        case 7: seg(0, 0, 26, 0); seg(26, 0, 26, 48); break;
        case 8: seg(0, 48, 26, 48); seg(26, 0, 26, 48); break;
        case 9: seg(0, 0, 26, 0); seg(0, 48, 26, 48); seg(26, 0, 26, 48); break;
        default: break;
    }
}

// Full numeral for `value` (1..9999) in font units: the stave at
// x = 32 spanning y -36..60, and the four digit quadrants around it:
// units upper right, tens upper left (horizontal mirror), hundreds
// lower right (vertical mirror), thousands lower left (both).
void systSegments(int value, std::vector<Seg>& out) {
    out.push_back({{32.0f, -36.0f}, {32.0f, 60.0f}}); // the stave
    struct Quadrant { int digit; float sx; float sy; float ox; float oy; };
    // (u, v) -> (ox + sx * u, oy + sy * v)
    const Quadrant quads[4] = {
        {value % 10, 1.0f, -1.0f, 32.0f, 60.0f},          // units
        {(value / 10) % 10, -1.0f, -1.0f, 32.0f, 60.0f},  // tens
        {(value / 100) % 10, 1.0f, 1.0f, 32.0f, -36.0f},  // hundreds
        {(value / 1000) % 10, -1.0f, 1.0f, 32.0f, -36.0f} // thousands
    };
    for (const Quadrant& q : quads) {
        if (q.digit == 0) continue;
        std::vector<Seg> local;
        systDigitSegs(q.digit, local);
        for (const Seg& s : local) {
            out.push_back({{q.ox + q.sx * s.first.x, q.oy + q.sy * s.first.y},
                           {q.ox + q.sx * s.second.x, q.oy + q.sy * s.second.y}});
        }
    }
}

float segPointDist(Vec2 p, Vec2 a, Vec2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float wx = p.x - a.x, wy = p.y - a.y;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0.0f ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

// ------------------------------------------------------------- wedges
// Sumerian (BuiltinFace::SumerianWedge): the Latin skeleton above,
// baked as cuneiform. Naming history, recorded honestly: this face
// was built on 2026-11-02 under the name "Systerian Text" because
// the transcription software substituted "Sumerian" for JD's word
// "Systerian"; JD renamed it to its honest name the same night.
// The face itself - geometry, bake, behavior - is unchanged.
// Each skeleton segment is chopped into wedge impressions - filled
// triangles, wide head tapering to a point tail - the way a reed
// stylus pressed into clay leaves one impression per stroke piece
// and a sign is a cluster of them.
//
// Proportions, in font units, off the face's nominal stroke width
// W = the line face's stroke diameter (2 * radius: 9 regular,
// 13 bold):
//   impression length: at most 1.8 * W (a segment is subdivided
//     evenly, so impressions along one stroke are uniform);
//   head width: 1.6 * W (the wide end of the triangle).
// Head orientation is GEOMETRIC, not path order: the Latin table
// authors verticals bottom-up as often as top-down, so the head
// goes to the impression's left end (horizontal runs) or top end
// (vertical runs), dominant axis deciding diagonals - vertical
// wedges head-up, horizontal wedges head-left, the classic
// cuneiform convention the face is named for.
struct WedgeTri {
    Vec2 headA; // head edge corners (headA/headB), tail the point
    Vec2 headB;
    Vec2 tail;
};

void wedgeTriangles(const std::vector<Seg>& segs, float maxLen, float headHalf,
                    std::vector<WedgeTri>& out) {
    for (const Seg& s : segs) {
        const float dx = s.second.x - s.first.x;
        const float dy = s.second.y - s.first.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f) continue; // a point carries no impression
        const int n = std::max(1, static_cast<int>(std::ceil(len / maxLen)));
        // Head end of the whole segment (see the convention above);
        // every impression in the segment shares the orientation.
        const bool headAtFirst =
            std::fabs(dx) >= std::fabs(dy) ? (dx > 0.0f) : (dy < 0.0f);
        const float ux = dx / len, uy = dy / len;
        const float px = -uy, py = ux; // unit perpendicular
        for (int k = 0; k < n; ++k) {
            const float t0 = static_cast<float>(k) / static_cast<float>(n);
            const float t1 = static_cast<float>(k + 1) / static_cast<float>(n);
            const Vec2 a{s.first.x + dx * t0, s.first.y + dy * t0};
            const Vec2 b{s.first.x + dx * t1, s.first.y + dy * t1};
            const Vec2& headC = headAtFirst ? a : b;
            const Vec2& tail = headAtFirst ? b : a;
            out.push_back({{headC.x + px * headHalf, headC.y + py * headHalf},
                           {headC.x - px * headHalf, headC.y - py * headHalf},
                           tail});
        }
    }
}

// Signed distance to a filled triangle: negative inside, so the
// bake's coverage is clamp(0.5 - d) exactly like the line face's
// clamp(radius + 0.5 - d).
float triPointDist(Vec2 p, const WedgeTri& t) {
    auto cross = [](Vec2 a, Vec2 b, Vec2 q) {
        return (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x);
    };
    const float c1 = cross(t.headA, t.headB, p);
    const float c2 = cross(t.headB, t.tail, p);
    const float c3 = cross(t.tail, t.headA, p);
    const bool inside = (c1 >= 0.0f && c2 >= 0.0f && c3 >= 0.0f) ||
                        (c1 <= 0.0f && c2 <= 0.0f && c3 <= 0.0f);
    const float d = std::min({segPointDist(p, t.headA, t.headB),
                              segPointDist(p, t.headB, t.tail),
                              segPointDist(p, t.tail, t.headA)});
    return inside ? -d : d;
}

// ------------------------------------------------- Systerian Text
// The true Systerian text face (JD's corrected design, 2026-11-02;
// he speaks the name as "Lithoglyphs"): Latin letterforms
// hand-authored in the Systerian NUMERALS' own construction
// language, so alphabet and numerals form one script family.
// No wedges, no Sumerian look, and NOT derived from the Latin
// skeleton table.
//
// The numeral kit this is built from (extracted from systSegments
// and systDigitSegs above): a vertical stave at x = 32 spanning
// y -36..60; digit quadrants 26 wide x 48 tall around it; a digit's
// strokes are only ever the two horizontal edges, the two
// corner-to-corner diagonals, or the outer vertical of its 26x48
// quadrant rectangle - full-span, edge-to-edge; the numeral body
// is 52 wide inside a 64 advance.
//
// ANGULAR REVISION (JD's ruling, 2026-11-02, after the first
// proof): LESS roundedness - sharp right angles and triangle
// angles, like the M's sharp V; the S must read as sharpened
// triangles turning at the angle of the staves, not paired
// arcs. So this face is built from straight segments ONLY,
// under one angle law:
//
//   THE STAVE ANGLE. Every cut, chamfer and zigzag diagonal
//   turns at the numeral quadrant's own diagonal slope,
//   dx:dy = 26:48 (= 13:24), or its mirror. The workhorse
//   chamfer is (13, 24) - exactly half the quadrant diagonal.
//   Diagonals over other spans take dx = round(dy * 26/48),
//   within one unit of the exact slope: the S zigzag legs and
//   R's leg (19 over 35), 2's long fall (25 over 46), the
//   lowercase s legs (14 over 26), Q's tail (9 over 16), the
//   descender hooks of g and j (8 over 14). Structural
//   diagonals a silhouette cannot give up keep their slopes
//   and are the documented exceptions: the corner-to-corner
//   diagonals of N, X, Z and K's legs (full-span in their
//   frames, like the numerals' own), the meeting diagonals
//   of M, V, W, Y (M's V is the sharpness JD cited as the
//   model), and small silhouette diagonals in punctuation
//   (?, the f hook, the slash family). A's legs ARE
//   stave-true: (26 over 48) down from the apex, then
//   vertical stilts to the baseline; 7's diagonal is the
//   stave slope over the cap height (38 over 70).
//
// Constructions, all in the shared em frame (baseline 0, cap
// height 70, x-height 52, descenders to -20, Latin metrics,
// so the face mixes on Latin lines):
//   bowls: chamfered frames - stem + top/bottom bars +
//     diagonal shoulders cut at the stave angle. O/0 is the
//     full frame; C the frame opened on the right; G the
//     frame plus its spur; B/P/R/3 are half-bowls (35 tall)
//     sharing the middle bar; 8 is two stacked frames; 6
//     and 9 are a hexagonal frame (48 tall) plus a stave
//     diagonal, exact 180-degree rotations of each other;
//     b/d and p/q are exact mirror pairs.
//   S/s: a sharpened zigzag - top bar, a falling stave
//     diagonal, a middle bar, a mirror stave diagonal,
//     bottom bar. The turns are the triangles JD asked for.
//   arches (h n m): a stave chamfer rising from the stem
//     to a flat top bar, then the leg - peaked shoulders,
//     no semicircles. U's bottom and u's bottom are stave
//     cuts joining the legs; J and g end in stave hooks.
// The bake is the numerals' own monoline line bake (same
// radius, same signed-distance coverage). The arc primitive
// added for this face's first (rounded) authoring stays in
// the baker - primPointDist/primBounds still handle it -
// but after this revision no face authors arcs: this face
// is all segments, and the Latin and numeral paths never
// used them.
// Advances: each advance is the glyph's authored span plus
// its bearing, widened by JD's spacing correction
// (2026-11-02, "the spacing was too close"): the bearing
// grew from +12 total to +20 total, so every advance in
// the table below is exactly +8 over the first authoring
// (space 30 -> 38), snapped even. Wording corrected in
// round-3 review FNT-2: "span + 20" is exact for the S/s
// constructions the correction was about, but it is NOT a
// universal table rule - digits are a uniform 72 by design
// and a few narrow glyphs (I, i, l, J) carry extra side
// bearing for legibility. The advances are authored per
// glyph; do not re-derive them from spans.
struct SystPrim {
    bool arc = false;
    Vec2 a{0, 0}, b{0, 0}; // segment endpoints (line prims)
    Vec2 c{0, 0};          // arc center (arc prims)
    float r = 0.0f;        // arc radius (arc prims)
    float a0 = 0.0f;       // arc start angle, degrees (arc prims)
    float sweep = 0.0f;    // arc sweep, degrees, signed (+ = CCW, y-up)
};

#define SPL(x1, y1, x2, y2) \
    SystPrim { false, {x1, y1}, {x2, y2}, {0, 0}, 0.0f, 0.0f, 0.0f }
#define SPA(cx, cy, r_, a0_, sw_) \
    SystPrim { true, {0, 0}, {0, 0}, {cx, cy}, r_, a0_, sw_ }

struct SystGlyphDef {
    char32_t cp;
    int adv; // advance in font units (see the bearing note above)
    const SystPrim* prims;
    int count;
};

#define SG(cp_, adv_, arr_) \
    SystGlyphDef { cp_, adv_, arr_, static_cast<int>(sizeof(arr_) / sizeof((arr_)[0])) }

// Punctuation and symbols.
const SystPrim sBang[] = {SPL(14, 26, 14, 70), SPL(14, 0, 14, 6)};
const SystPrim sDQuote[] = {SPL(10, 56, 10, 70), SPL(22, 56, 22, 70)};
const SystPrim sHash[] = {SPL(20, 0, 26, 70), SPL(42, 0, 48, 70),
                          SPL(6, 24, 56, 24), SPL(6, 46, 56, 46)};
const SystPrim sDollar[] = {SPL(12, 60, 42, 60), SPL(42, 60, 13, 6),
                            SPL(13, 6, 42, 6), SPL(27, -8, 27, 78)};
const SystPrim sPercent[] = {SPL(4, 58, 10.5f, 70), SPL(10.5f, 70, 21.5f, 70),
                             SPL(21.5f, 70, 28, 58), SPL(28, 58, 21.5f, 46),
                             SPL(21.5f, 46, 10.5f, 46), SPL(10.5f, 46, 4, 58),
                             SPL(66, 4, 18, 70),
                             SPL(52, 12, 58.5f, 24), SPL(58.5f, 24, 69.5f, 24),
                             SPL(69.5f, 24, 76, 12), SPL(76, 12, 69.5f, 0),
                             SPL(69.5f, 0, 58.5f, 0), SPL(58.5f, 0, 52, 12)};
const SystPrim sAmp[] = {SPL(14, 57, 20, 68), SPL(20, 68, 38, 68),
                         SPL(38, 68, 44, 57), SPL(44, 57, 38, 46),
                         SPL(38, 46, 20, 46), SPL(20, 46, 14, 57),
                         SPL(38, 46, 48, 28), SPL(48, 28, 42, 6),
                         SPL(42, 6, 26, 0), SPL(26, 0, 10, 6),
                         SPL(10, 6, 6, 20), SPL(26, 24, 52, 60)};
const SystPrim sSQuote[] = {SPL(10, 56, 10, 70)};
const SystPrim sLParen[] = {SPL(22, 66, 5, 35), SPL(5, 35, 22, 4)};
const SystPrim sRParen[] = {SPL(10, 66, 27, 35), SPL(27, 35, 10, 4)};
const SystPrim sStar[] = {SPL(22, 36, 22, 64), SPL(10, 42, 34, 58), SPL(10, 58, 34, 42)};
const SystPrim sPlus[] = {SPL(26, 12, 26, 58), SPL(6, 35, 46, 35)};
const SystPrim sComma[] = {SPL(14, 1, 14, 8), SPL(14, 1, 7, -9)};
const SystPrim sMinus[] = {SPL(8, 32, 32, 32)};
const SystPrim sDot[] = {SPL(14, 0, 14, 6)};
const SystPrim sSlash[] = {SPL(34, 0, 6, 70)};
const SystPrim sColon[] = {SPL(14, 40, 14, 47), SPL(14, 10, 14, 17)};
const SystPrim sSemi[] = {SPL(14, 40, 14, 47), SPL(14, 11, 14, 17), SPL(14, 11, 8, -1)};
const SystPrim sLt[] = {SPL(44, 56, 10, 32), SPL(10, 32, 44, 8)};
const SystPrim sEq[] = {SPL(6, 42, 42, 42), SPL(6, 22, 42, 22)};
const SystPrim sGt[] = {SPL(8, 56, 42, 32), SPL(42, 32, 8, 8)};
const SystPrim sQuest[] = {SPL(12, 52, 24, 70), SPL(24, 70, 44, 70),
                           SPL(44, 70, 56, 52), SPL(56, 52, 56, 40),
                           SPL(56, 40, 26, 28), SPL(26, 28, 26, 14),
                           SPL(26, 0, 26, 6)};
const SystPrim sAt[] = {SPL(8, 27, 8, 39), SPL(8, 39, 21, 63),
                        SPL(21, 63, 55, 63), SPL(55, 63, 68, 39),
                        SPL(68, 39, 68, 27), SPL(68, 27, 55, 3),
                        SPL(55, 3, 21, 3), SPL(21, 3, 8, 27),
                        SPL(24, 36, 30.5f, 48), SPL(30.5f, 48, 52, 48),
                        SPL(52, 48, 52, 18), SPL(52, 18, 30.5f, 18),
                        SPL(30.5f, 18, 24, 30), SPL(24, 30, 24, 36)};
const SystPrim sLBrack[] = {SPL(24, 76, 10, 76), SPL(10, 76, 10, -6), SPL(10, -6, 24, -6)};
const SystPrim sBSlash[] = {SPL(6, 70, 34, 0)};
const SystPrim sRBrack[] = {SPL(8, 76, 22, 76), SPL(22, 76, 22, -6), SPL(22, -6, 8, -6)};
const SystPrim sCaret[] = {SPL(8, 42, 22, 60), SPL(22, 60, 36, 42)};
const SystPrim sUnder[] = {SPL(4, -12, 44, -12)};
const SystPrim sGrave[] = {SPL(9, 70, 17, 56)};
const SystPrim sLBrace[] = {SPL(26, 76, 18, 68), SPL(18, 68, 16, 52), SPL(16, 52, 16, 44),
                            SPL(16, 44, 8, 36), SPL(8, 36, 16, 28), SPL(16, 28, 16, 14),
                            SPL(16, 14, 20, 2), SPL(20, 2, 26, -6)};
const SystPrim sBar[] = {SPL(12, -6, 12, 76)};
const SystPrim sRBrace[] = {SPL(10, 76, 18, 68), SPL(18, 68, 20, 52), SPL(20, 52, 20, 44),
                            SPL(20, 44, 28, 36), SPL(28, 36, 20, 28), SPL(20, 28, 20, 14),
                            SPL(20, 14, 16, 2), SPL(16, 2, 10, -6)};
const SystPrim sTilde[] = {SPL(6, 32, 15, 41), SPL(15, 41, 24, 32),
                           SPL(24, 32, 33, 23), SPL(33, 23, 42, 32)};

// Digits: Latin digit silhouettes in this construction (the
// composite Cistercian numerals stay at U+F0000+value).
const SystPrim sD0[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24)};
const SystPrim sD1[] = {SPL(38, 0, 38, 70), SPL(14, 46, 38, 70), SPL(14, 0, 58, 0)};
const SystPrim sD2[] = {SPL(8, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 33, 0), SPL(8, 0, 58, 0)};
const SystPrim sD3[] = {SPL(10, 70, 41, 70), SPL(41, 70, 54, 46),
                        SPL(54, 46, 54, 35), SPL(12, 35, 54, 35),
                        SPL(54, 35, 54, 24), SPL(54, 24, 41, 0),
                        SPL(10, 0, 41, 0)};
const SystPrim sD4[] = {SPL(42, 0, 42, 70), SPL(42, 70, 10, 16), SPL(6, 16, 58, 16)};
const SystPrim sD5[] = {SPL(10, 70, 56, 70), SPL(10, 35, 10, 70),
                        SPL(10, 35, 54, 35), SPL(54, 35, 54, 24),
                        SPL(54, 24, 41, 0), SPL(10, 0, 41, 0)};
const SystPrim sD6[] = {SPL(57, 70, 45, 48), SPL(19, 48, 45, 48),
                        SPL(45, 48, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24),
                        SPL(6, 24, 19, 48)};
const SystPrim sD7[] = {SPL(8, 70, 56, 70), SPL(56, 70, 18, 0)};
const SystPrim sD8[] = {SPL(12, 35, 12, 46), SPL(12, 46, 25, 70),
                        SPL(25, 70, 39, 70), SPL(39, 70, 52, 46),
                        SPL(52, 46, 52, 35), SPL(12, 35, 52, 35),
                        SPL(12, 24, 12, 35), SPL(12, 24, 25, 0),
                        SPL(25, 0, 39, 0), SPL(39, 0, 52, 24),
                        SPL(52, 24, 52, 35)};
const SystPrim sD9[] = {SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 45, 22), SPL(45, 22, 19, 22),
                        SPL(19, 22, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 22, 7, 0)};

// Uppercase.
const SystPrim sUA[] = {SPL(32, 70, 6, 22), SPL(32, 70, 58, 22),
                        SPL(6, 22, 6, 0), SPL(58, 22, 58, 0),
                        SPL(6, 22, 58, 22)};
const SystPrim sUB[] = {SPL(6, 0, 6, 70), SPL(6, 70, 41, 70),
                        SPL(41, 70, 54, 46), SPL(54, 46, 54, 35),
                        SPL(54, 35, 6, 35), SPL(54, 35, 54, 24),
                        SPL(54, 24, 41, 0), SPL(41, 0, 6, 0)};
const SystPrim sUC[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 24, 45, 0), SPL(45, 0, 19, 0),
                        SPL(19, 0, 6, 24)};
const SystPrim sUD[] = {SPL(6, 0, 6, 70), SPL(6, 70, 45, 70),
                        SPL(45, 70, 58, 46), SPL(58, 46, 58, 24),
                        SPL(58, 24, 45, 0), SPL(45, 0, 6, 0)};
const SystPrim sUE[] = {SPL(6, 0, 6, 70), SPL(6, 70, 56, 70), SPL(6, 35, 48, 35),
                        SPL(6, 0, 56, 0)};
const SystPrim sUF[] = {SPL(6, 0, 6, 70), SPL(6, 70, 52, 70), SPL(6, 35, 44, 35)};
const SystPrim sUG[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 0), SPL(58, 0, 19, 0),
                        SPL(19, 0, 6, 24), SPL(36, 35, 58, 35)};
const SystPrim sUH[] = {SPL(6, 0, 6, 70), SPL(58, 0, 58, 70), SPL(6, 35, 58, 35)};
const SystPrim sUI[] = {SPL(19, 0, 19, 70), SPL(10, 70, 28, 70), SPL(10, 0, 28, 0)};
const SystPrim sUJ[] = {SPL(54, 24, 54, 70), SPL(54, 24, 41, 0),
                        SPL(14, 0, 41, 0)};
const SystPrim sUK[] = {SPL(6, 0, 6, 70), SPL(58, 70, 6, 32), SPL(6, 32, 58, 0)};
const SystPrim sUL[] = {SPL(6, 0, 6, 70), SPL(6, 0, 56, 0)};
const SystPrim sUM[] = {SPL(6, 0, 6, 70), SPL(84, 0, 84, 70), SPL(6, 70, 45, 24),
                        SPL(84, 70, 45, 24)};
const SystPrim sUN[] = {SPL(6, 0, 6, 70), SPL(58, 0, 58, 70), SPL(6, 70, 58, 0)};
const SystPrim sUO[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24)};
const SystPrim sUP[] = {SPL(6, 0, 6, 70), SPL(6, 70, 41, 70),
                        SPL(41, 70, 54, 46), SPL(54, 46, 54, 35),
                        SPL(54, 35, 6, 35)};
const SystPrim sUQ[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24),
                        SPL(51, 12, 60, -4)};
const SystPrim sUR[] = {SPL(6, 0, 6, 70), SPL(6, 70, 41, 70),
                        SPL(41, 70, 54, 46), SPL(54, 46, 54, 35),
                        SPL(54, 35, 6, 35), SPL(35, 35, 54, 0)};
const SystPrim sUS[] = {SPL(12, 70, 56, 70), SPL(56, 70, 37, 35),
                        SPL(37, 35, 19, 35), SPL(19, 35, 38, 0),
                        SPL(12, 0, 56, 0)};
const SystPrim sUT[] = {SPL(32, 0, 32, 70), SPL(6, 70, 58, 70)};
const SystPrim sUU[] = {SPL(6, 24, 6, 70), SPL(6, 24, 19, 0),
                        SPL(19, 0, 45, 0), SPL(45, 0, 58, 24),
                        SPL(58, 24, 58, 70)};
const SystPrim sUV[] = {SPL(6, 70, 32, 0), SPL(58, 70, 32, 0)};
const SystPrim sUW[] = {SPL(6, 70, 24, 0), SPL(24, 0, 45, 46), SPL(45, 46, 66, 0),
                        SPL(66, 0, 84, 70)};
const SystPrim sUX[] = {SPL(6, 0, 58, 70), SPL(6, 70, 58, 0)};
const SystPrim sUY[] = {SPL(6, 70, 32, 32), SPL(58, 70, 32, 32), SPL(32, 0, 32, 32)};
const SystPrim sUZ[] = {SPL(6, 70, 58, 70), SPL(58, 70, 6, 0), SPL(6, 0, 58, 0)};

// Lowercase (x-height 52).
const SystPrim sLa[] = {SPL(6, 28, 19, 52), SPL(19, 52, 58, 52),
                        SPL(58, 52, 58, 0), SPL(58, 0, 19, 0),
                        SPL(19, 0, 6, 24), SPL(6, 24, 6, 28)};
const SystPrim sLb[] = {SPL(6, 0, 6, 70), SPL(6, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(58, 28, 58, 24),
                        SPL(58, 24, 45, 0), SPL(45, 0, 6, 0)};
const SystPrim sLc[] = {SPL(6, 28, 19, 52), SPL(19, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(58, 14, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24),
                        SPL(6, 24, 6, 28)};
const SystPrim sLd[] = {SPL(58, 0, 58, 70), SPL(19, 52, 58, 52),
                        SPL(19, 52, 6, 28), SPL(6, 28, 6, 24),
                        SPL(6, 24, 19, 0), SPL(19, 0, 58, 0)};
const SystPrim sLe[] = {SPL(6, 28, 19, 52), SPL(19, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(6, 28, 58, 28),
                        SPL(6, 24, 6, 28), SPL(19, 0, 6, 24),
                        SPL(19, 0, 45, 0)};
const SystPrim sLf[] = {SPL(26, 70, 42, 70), SPL(15, 58, 26, 70),
                        SPL(15, 0, 15, 58), SPL(7, 38, 35, 38)};
const SystPrim sLg[] = {SPL(6, 28, 19, 52), SPL(19, 52, 58, 52),
                        SPL(58, 52, 58, -6), SPL(58, 0, 19, 0),
                        SPL(19, 0, 6, 24), SPL(6, 24, 6, 28),
                        SPL(58, -6, 50, -20), SPL(36, -20, 50, -20)};
const SystPrim sLh[] = {SPL(6, 0, 6, 70), SPL(6, 28, 19, 52),
                        SPL(19, 52, 58, 52), SPL(58, 52, 58, 0)};
const SystPrim sLi[] = {SPL(19, 0, 19, 52), SPL(19, 63, 19, 69)};
const SystPrim sLj[] = {SPL(38, -6, 38, 52), SPL(38, -6, 30, -20),
                        SPL(16, -20, 30, -20), SPL(38, 63, 38, 69)};
const SystPrim sLk[] = {SPL(6, 0, 6, 70), SPL(48, 52, 8, 22), SPL(14, 28, 50, 0)};
const SystPrim sLl[] = {SPL(19, 0, 19, 70)};
const SystPrim sLm[] = {SPL(6, 0, 6, 52), SPL(6, 28, 19, 52),
                        SPL(19, 52, 45, 52), SPL(45, 0, 45, 52),
                        SPL(45, 28, 58, 52), SPL(58, 52, 84, 52),
                        SPL(84, 52, 84, 0)};
const SystPrim sLn[] = {SPL(6, 0, 6, 52), SPL(6, 28, 19, 52),
                        SPL(19, 52, 58, 52), SPL(58, 52, 58, 0)};
const SystPrim sLo[] = {SPL(6, 24, 6, 28), SPL(6, 28, 19, 52),
                        SPL(19, 52, 45, 52), SPL(45, 52, 58, 28),
                        SPL(58, 28, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24)};
const SystPrim sLp[] = {SPL(6, -20, 6, 52), SPL(6, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(58, 28, 58, 24),
                        SPL(58, 24, 45, 0), SPL(45, 0, 6, 0)};
const SystPrim sLq[] = {SPL(58, -20, 58, 52), SPL(19, 52, 58, 52),
                        SPL(19, 52, 6, 28), SPL(6, 28, 6, 24),
                        SPL(6, 24, 19, 0), SPL(19, 0, 58, 0)};
const SystPrim sLr[] = {SPL(6, 0, 6, 52), SPL(6, 52, 28, 52),
                        SPL(28, 52, 36, 38)};
const SystPrim sLs[] = {SPL(14, 52, 52, 52), SPL(52, 52, 38, 26),
                        SPL(38, 26, 22, 26), SPL(22, 26, 36, 0),
                        SPL(14, 0, 52, 0)};
const SystPrim sLowT[] = {SPL(20, 0, 20, 70), SPL(8, 38, 36, 38),
                          SPL(20, 0, 36, 0)};
const SystPrim sLu[] = {SPL(6, 24, 6, 52), SPL(6, 24, 19, 0),
                        SPL(19, 0, 45, 0), SPL(45, 0, 58, 24),
                        SPL(58, 0, 58, 52)};
const SystPrim sLv[] = {SPL(6, 52, 32, 0), SPL(58, 52, 32, 0)};
const SystPrim sLw[] = {SPL(6, 52, 22, 0), SPL(22, 0, 42, 36), SPL(42, 36, 62, 0),
                        SPL(62, 0, 84, 52)};
const SystPrim sLx[] = {SPL(8, 0, 48, 52), SPL(8, 52, 48, 0)};
const SystPrim sLy[] = {SPL(6, 52, 24, 14), SPL(48, 52, 24, 14), SPL(24, 14, 14, -16)};
const SystPrim sLz[] = {SPL(8, 52, 48, 52), SPL(48, 52, 8, 0), SPL(8, 0, 48, 0)};

const SystGlyphDef kSystTextGlyphs[] = {
    SystGlyphDef{U' ', 38, nullptr, 0},
    SG(U'!', 36, sBang), SG(U'"', 40, sDQuote), SG(U'#', 70, sHash),
    SG(U'$', 64, sDollar), SG(U'%', 92, sPercent), SG(U'&', 70, sAmp),
    SG(U'\'', 28, sSQuote), SG(U'(', 40, sLParen), SG(U')', 40, sRParen),
    SG(U'*', 52, sStar), SG(U'+', 60, sPlus), SG(U',', 36, sComma),
    SG(U'-', 48, sMinus), SG(U'.', 36, sDot), SG(U'/', 48, sSlash),
    SG(U'0', 72, sD0), SG(U'1', 72, sD1), SG(U'2', 72, sD2), SG(U'3', 72, sD3),
    SG(U'4', 72, sD4), SG(U'5', 72, sD5), SG(U'6', 72, sD6), SG(U'7', 72, sD7),
    SG(U'8', 72, sD8), SG(U'9', 72, sD9),
    SG(U':', 36, sColon), SG(U';', 36, sSemi), SG(U'<', 60, sLt),
    SG(U'=', 56, sEq), SG(U'>', 60, sGt), SG(U'?', 60, sQuest),
    SG(U'@', 84, sAt),
    SG(U'A', 72, sUA), SG(U'B', 68, sUB), SG(U'C', 72, sUC), SG(U'D', 72, sUD),
    SG(U'E', 70, sUE), SG(U'F', 66, sUF), SG(U'G', 72, sUG), SG(U'H', 72, sUH),
    SG(U'I', 46, sUI), SG(U'J', 72, sUJ), SG(U'K', 72, sUK), SG(U'L', 70, sUL),
    SG(U'M', 98, sUM), SG(U'N', 72, sUN), SG(U'O', 72, sUO), SG(U'P', 68, sUP),
    SG(U'Q', 72, sUQ), SG(U'R', 72, sUR), SG(U'S', 64, sUS), SG(U'T', 72, sUT),
    SG(U'U', 72, sUU), SG(U'V', 72, sUV), SG(U'W', 98, sUW), SG(U'X', 72, sUX),
    SG(U'Y', 72, sUY), SG(U'Z', 72, sUZ),
    SG(U'[', 40, sLBrack), SG(U'\\', 48, sBSlash), SG(U']', 40, sRBrack),
    SG(U'^', 52, sCaret), SG(U'_', 56, sUnder), SG(U'`', 32, sGrave),
    SG(U'a', 72, sLa), SG(U'b', 72, sLb), SG(U'c', 72, sLc), SG(U'd', 72, sLd),
    SG(U'e', 72, sLe), SG(U'f', 52, sLf), SG(U'g', 72, sLg), SG(U'h', 72, sLh),
    SG(U'i', 46, sLi), SG(U'j', 52, sLj), SG(U'k', 66, sLk), SG(U'l', 46, sLl),
    SG(U'm', 98, sLm), SG(U'n', 72, sLn), SG(U'o', 72, sLo), SG(U'p', 72, sLp),
    SG(U'q', 72, sLq), SG(U'r', 52, sLr), SG(U's', 58, sLs), SG(U't', 52, sLowT),
    SG(U'u', 72, sLu), SG(U'v', 72, sLv), SG(U'w', 98, sLw), SG(U'x', 64, sLx),
    SG(U'y', 64, sLy), SG(U'z', 64, sLz),
    SG(U'{', 44, sLBrace), SG(U'|', 32, sBar), SG(U'}', 44, sRBrace),
    SG(U'~', 56, sTilde),
};
// ------------------------------------------------- Systerian Break
// The readable variation of Systerian Text (JD's ruling,
// 2026-11-02, after the angular proof): JD likes the angular
// face and its unique feel, but the zigzag S is really really
// hard to read, and for this variation READABILITY WINS. The
// angular face (BuiltinFace::SysterianText) stays exactly as
// it is - the "hard" variation; this face carries its
// constructions over glyph for glyph with two changes:
//
//   1. A READABLE S. S/s is built in the universally readable
//      segmented form - top bar, upper-left stem, middle bar,
//      lower-right stem, bottom bar (the seven-segment S
//      path, the same path the angular face's digit 5 already
//      uses) - with stave-angle chamfer cuts at the two free
//      corners (10 over 18 in the capitals, 7 over 13 in the
//      lowercase, by the face's dx = round(dy * 26/48) rule)
//      and exact 180-degree rotational symmetry.
//
//   2. BREAKS IN THE STAVES, the variation's signature.
//      Every primary full-height stem is split by ONE clean
//      gap at the QUADRANT SEAM: y = 35, the midpoint of the
//      stem span 0..70, mirroring the numeral stave's own
//      midline (y = 12, the midpoint of its -36..60 span)
//      where its upper and lower quadrants meet. The gap is
//      sized so the CLEAR band at the stem column is about
//      one stroke diameter (9 units) tall: the bake's ink
//      reaches radius + 0.5 = 5 units past a skeleton end,
//      so the skeleton gap is 19 units (stems end at 25.5
//      and resume at 44.5). Measured on this baker, the
//      band at font y 32..39 (8 texel rows) bakes to hard
//      zero, flanked by one faint anti-aliased fringe row
//      on each side from the cut ends - the smoke suite
//      pins all of it. At bold (radius 6.5) the clear band
//      narrows; the break still reads.
//
// THE BREAK RULE, exactly as implemented (the tables below
// are the angular face's tables with the rule applied):
// a vertical segment is split at the seam iff
//   (a) it is a primary stem (stave) of the glyph - bowl
//       frames (O C G Q 0, the bowl sides of a c e g o p q),
//       bars, diagonals, hooks and dots never split, so
//       bowls and bars stay continuous;
//   (b) it passes through the whole skeleton gap with stem
//       material on both sides (a stem ending inside the
//       seam zone, like Y's, is not split);
//   (c) no horizontal bar joins or crosses the stem within
//       the seam zone - where a bar already joints the stem
//       at the seam (B E F H P R at their y = 35 middle
//       bars; f and t at their y = 38 crossbars), the bar
//       IS the joint and the stem stays whole;
//   (d) no diagonal joins the stem within the seam zone
//       (K, whose diagonals join at (6, 32)).
// Scope: capitals, lowercase ascenders (b d h k l), and the
// same-shaped stems of J, T and digits 1 and 4. X-height
// stems do not split: the seam is the full-height
// construction's joint, not theirs. Punctuation and
// symbols carry over unchanged.
// Split stems: D I J L M N T U, digits 1 4, b d h k l
// (U's and J's legs split like the rest; their foot
// junctions sit at y = 24, just below the gap, and the
// short lower piece's ink merges with the foot's).
// Advances are the angular face's advances, carried over
// unchanged for every glyph except the rebuilt S (advance
// 68) and s (60) - theirs follow span + 20 on the new spans
// (48 -> 68, 40 -> 60); the smoke suite pins advance parity
// for all the rest. (Round-3 review FNT-2 wording fix: the
// claim is parity with the angular face, not "span + 20"
// as a general rule - see the angular face's advance note.)

const SystPrim bBang[] = {SPL(14, 26, 14, 70), SPL(14, 0, 14, 6)};
const SystPrim bDQuote[] = {SPL(10, 56, 10, 70), SPL(22, 56, 22, 70)};
const SystPrim bHash[] = {SPL(20, 0, 26, 70), SPL(42, 0, 48, 70),
                          SPL(6, 24, 56, 24), SPL(6, 46, 56, 46)};
const SystPrim bDollar[] = {SPL(12, 60, 42, 60), SPL(42, 60, 13, 6),
                            SPL(13, 6, 42, 6), SPL(27, -8, 27, 78)};
const SystPrim bPercent[] = {SPL(4, 58, 10.5f, 70), SPL(10.5f, 70, 21.5f, 70),
                             SPL(21.5f, 70, 28, 58), SPL(28, 58, 21.5f, 46),
                             SPL(21.5f, 46, 10.5f, 46), SPL(10.5f, 46, 4, 58),
                             SPL(66, 4, 18, 70),
                             SPL(52, 12, 58.5f, 24), SPL(58.5f, 24, 69.5f, 24),
                             SPL(69.5f, 24, 76, 12), SPL(76, 12, 69.5f, 0),
                             SPL(69.5f, 0, 58.5f, 0), SPL(58.5f, 0, 52, 12)};
const SystPrim bAmp[] = {SPL(14, 57, 20, 68), SPL(20, 68, 38, 68),
                         SPL(38, 68, 44, 57), SPL(44, 57, 38, 46),
                         SPL(38, 46, 20, 46), SPL(20, 46, 14, 57),
                         SPL(38, 46, 48, 28), SPL(48, 28, 42, 6),
                         SPL(42, 6, 26, 0), SPL(26, 0, 10, 6),
                         SPL(10, 6, 6, 20), SPL(26, 24, 52, 60)};
const SystPrim bSQuote[] = {SPL(10, 56, 10, 70)};
const SystPrim bLParen[] = {SPL(22, 66, 5, 35), SPL(5, 35, 22, 4)};
const SystPrim bRParen[] = {SPL(10, 66, 27, 35), SPL(27, 35, 10, 4)};
const SystPrim bStar[] = {SPL(22, 36, 22, 64), SPL(10, 42, 34, 58), SPL(10, 58, 34, 42)};
const SystPrim bPlus[] = {SPL(26, 12, 26, 58), SPL(6, 35, 46, 35)};
const SystPrim bComma[] = {SPL(14, 1, 14, 8), SPL(14, 1, 7, -9)};
const SystPrim bMinus[] = {SPL(8, 32, 32, 32)};
const SystPrim bDot[] = {SPL(14, 0, 14, 6)};
const SystPrim bSlash[] = {SPL(34, 0, 6, 70)};
const SystPrim bColon[] = {SPL(14, 40, 14, 47), SPL(14, 10, 14, 17)};
const SystPrim bSemi[] = {SPL(14, 40, 14, 47), SPL(14, 11, 14, 17), SPL(14, 11, 8, -1)};
const SystPrim bLt[] = {SPL(44, 56, 10, 32), SPL(10, 32, 44, 8)};
const SystPrim bEq[] = {SPL(6, 42, 42, 42), SPL(6, 22, 42, 22)};
const SystPrim bGt[] = {SPL(8, 56, 42, 32), SPL(42, 32, 8, 8)};
const SystPrim bQuest[] = {SPL(12, 52, 24, 70), SPL(24, 70, 44, 70),
                           SPL(44, 70, 56, 52), SPL(56, 52, 56, 40),
                           SPL(56, 40, 26, 28), SPL(26, 28, 26, 14),
                           SPL(26, 0, 26, 6)};
const SystPrim bAt[] = {SPL(8, 27, 8, 39), SPL(8, 39, 21, 63),
                        SPL(21, 63, 55, 63), SPL(55, 63, 68, 39),
                        SPL(68, 39, 68, 27), SPL(68, 27, 55, 3),
                        SPL(55, 3, 21, 3), SPL(21, 3, 8, 27),
                        SPL(24, 36, 30.5f, 48), SPL(30.5f, 48, 52, 48),
                        SPL(52, 48, 52, 18), SPL(52, 18, 30.5f, 18),
                        SPL(30.5f, 18, 24, 30), SPL(24, 30, 24, 36)};
const SystPrim bLBrack[] = {SPL(24, 76, 10, 76), SPL(10, 76, 10, -6), SPL(10, -6, 24, -6)};
const SystPrim bBSlash[] = {SPL(6, 70, 34, 0)};
const SystPrim bRBrack[] = {SPL(8, 76, 22, 76), SPL(22, 76, 22, -6), SPL(22, -6, 8, -6)};
const SystPrim bCaret[] = {SPL(8, 42, 22, 60), SPL(22, 60, 36, 42)};
const SystPrim bUnder[] = {SPL(4, -12, 44, -12)};
const SystPrim bGrave[] = {SPL(9, 70, 17, 56)};
const SystPrim bLBrace[] = {SPL(26, 76, 18, 68), SPL(18, 68, 16, 52), SPL(16, 52, 16, 44),
                            SPL(16, 44, 8, 36), SPL(8, 36, 16, 28), SPL(16, 28, 16, 14),
                            SPL(16, 14, 20, 2), SPL(20, 2, 26, -6)};
const SystPrim bBar[] = {SPL(12, -6, 12, 76)};
const SystPrim bRBrace[] = {SPL(10, 76, 18, 68), SPL(18, 68, 20, 52), SPL(20, 52, 20, 44),
                            SPL(20, 44, 28, 36), SPL(28, 36, 20, 28), SPL(20, 28, 20, 14),
                            SPL(20, 14, 16, 2), SPL(16, 2, 10, -6)};
const SystPrim bTilde[] = {SPL(6, 32, 15, 41), SPL(15, 41, 24, 32),
                           SPL(24, 32, 33, 23), SPL(33, 23, 42, 32)};

// Digits: Latin digit silhouettes in this construction (the
// composite Cistercian numerals stay at U+F0000+value).
const SystPrim bD0[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24)};
const SystPrim bD1[] = {SPL(38, 0, 38, 25.5f), SPL(38, 44.5f, 38, 70), SPL(14, 46, 38, 70), SPL(14, 0, 58, 0)};
const SystPrim bD2[] = {SPL(8, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 33, 0), SPL(8, 0, 58, 0)};
const SystPrim bD3[] = {SPL(10, 70, 41, 70), SPL(41, 70, 54, 46),
                        SPL(54, 46, 54, 35), SPL(12, 35, 54, 35),
                        SPL(54, 35, 54, 24), SPL(54, 24, 41, 0),
                        SPL(10, 0, 41, 0)};
const SystPrim bD4[] = {SPL(42, 0, 42, 25.5f), SPL(42, 44.5f, 42, 70), SPL(42, 70, 10, 16), SPL(6, 16, 58, 16)};
const SystPrim bD5[] = {SPL(10, 70, 56, 70), SPL(10, 35, 10, 70),
                        SPL(10, 35, 54, 35), SPL(54, 35, 54, 24),
                        SPL(54, 24, 41, 0), SPL(10, 0, 41, 0)};
const SystPrim bD6[] = {SPL(57, 70, 45, 48), SPL(19, 48, 45, 48),
                        SPL(45, 48, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24),
                        SPL(6, 24, 19, 48)};
const SystPrim bD7[] = {SPL(8, 70, 56, 70), SPL(56, 70, 18, 0)};
const SystPrim bD8[] = {SPL(12, 35, 12, 46), SPL(12, 46, 25, 70),
                        SPL(25, 70, 39, 70), SPL(39, 70, 52, 46),
                        SPL(52, 46, 52, 35), SPL(12, 35, 52, 35),
                        SPL(12, 24, 12, 35), SPL(12, 24, 25, 0),
                        SPL(25, 0, 39, 0), SPL(39, 0, 52, 24),
                        SPL(52, 24, 52, 35)};
const SystPrim bD9[] = {SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 45, 22), SPL(45, 22, 19, 22),
                        SPL(19, 22, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 22, 7, 0)};

// Uppercase.
const SystPrim bUA[] = {SPL(32, 70, 6, 22), SPL(32, 70, 58, 22),
                        SPL(6, 22, 6, 0), SPL(58, 22, 58, 0),
                        SPL(6, 22, 58, 22)};
const SystPrim bUB[] = {SPL(6, 0, 6, 70), SPL(6, 70, 41, 70),
                        SPL(41, 70, 54, 46), SPL(54, 46, 54, 35),
                        SPL(54, 35, 6, 35), SPL(54, 35, 54, 24),
                        SPL(54, 24, 41, 0), SPL(41, 0, 6, 0)};
const SystPrim bUC[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 24, 45, 0), SPL(45, 0, 19, 0),
                        SPL(19, 0, 6, 24)};
const SystPrim bUD[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(6, 70, 45, 70),
                        SPL(45, 70, 58, 46), SPL(58, 46, 58, 24),
                        SPL(58, 24, 45, 0), SPL(45, 0, 6, 0)};
const SystPrim bUE[] = {SPL(6, 0, 6, 70), SPL(6, 70, 56, 70), SPL(6, 35, 48, 35),
                        SPL(6, 0, 56, 0)};
const SystPrim bUF[] = {SPL(6, 0, 6, 70), SPL(6, 70, 52, 70), SPL(6, 35, 44, 35)};
const SystPrim bUG[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 0), SPL(58, 0, 19, 0),
                        SPL(19, 0, 6, 24), SPL(36, 35, 58, 35)};
const SystPrim bUH[] = {SPL(6, 0, 6, 70), SPL(58, 0, 58, 70), SPL(6, 35, 58, 35)};
const SystPrim bUI[] = {SPL(19, 0, 19, 25.5f), SPL(19, 44.5f, 19, 70), SPL(10, 70, 28, 70), SPL(10, 0, 28, 0)};
const SystPrim bUJ[] = {SPL(54, 24, 54, 25.5f), SPL(54, 44.5f, 54, 70), SPL(54, 24, 41, 0),
                        SPL(14, 0, 41, 0)};
const SystPrim bUK[] = {SPL(6, 0, 6, 70), SPL(58, 70, 6, 32), SPL(6, 32, 58, 0)};
const SystPrim bUL[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(6, 0, 56, 0)};
const SystPrim bUM[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(84, 0, 84, 25.5f), SPL(84, 44.5f, 84, 70), SPL(6, 70, 45, 24),
                        SPL(84, 70, 45, 24)};
const SystPrim bUN[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(58, 0, 58, 25.5f), SPL(58, 44.5f, 58, 70), SPL(6, 70, 58, 0)};
const SystPrim bUO[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24)};
const SystPrim bUP[] = {SPL(6, 0, 6, 70), SPL(6, 70, 41, 70),
                        SPL(41, 70, 54, 46), SPL(54, 46, 54, 35),
                        SPL(54, 35, 6, 35)};
const SystPrim bUQ[] = {SPL(6, 24, 6, 46), SPL(6, 46, 19, 70),
                        SPL(19, 70, 45, 70), SPL(45, 70, 58, 46),
                        SPL(58, 46, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24),
                        SPL(51, 12, 60, -4)};
const SystPrim bUR[] = {SPL(6, 0, 6, 70), SPL(6, 70, 41, 70),
                        SPL(41, 70, 54, 46), SPL(54, 46, 54, 35),
                        SPL(54, 35, 6, 35), SPL(35, 35, 54, 0)};
const SystPrim bUS[] = {SPL(18, 70, 52, 70), SPL(8, 52, 18, 70),
                        SPL(8, 35, 8, 52), SPL(8, 35, 56, 35),
                        SPL(56, 18, 56, 35), SPL(46, 0, 56, 18),
                        SPL(12, 0, 46, 0)};
const SystPrim bUT[] = {SPL(32, 0, 32, 25.5f), SPL(32, 44.5f, 32, 70), SPL(6, 70, 58, 70)};
const SystPrim bUU[] = {SPL(6, 24, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(6, 24, 19, 0),
                        SPL(19, 0, 45, 0), SPL(45, 0, 58, 24),
                        SPL(58, 24, 58, 25.5f), SPL(58, 44.5f, 58, 70)};
const SystPrim bUV[] = {SPL(6, 70, 32, 0), SPL(58, 70, 32, 0)};
const SystPrim bUW[] = {SPL(6, 70, 24, 0), SPL(24, 0, 45, 46), SPL(45, 46, 66, 0),
                        SPL(66, 0, 84, 70)};
const SystPrim bUX[] = {SPL(6, 0, 58, 70), SPL(6, 70, 58, 0)};
const SystPrim bUY[] = {SPL(6, 70, 32, 32), SPL(58, 70, 32, 32), SPL(32, 0, 32, 32)};
const SystPrim bUZ[] = {SPL(6, 70, 58, 70), SPL(58, 70, 6, 0), SPL(6, 0, 58, 0)};

// Lowercase (x-height 52).
const SystPrim bLa[] = {SPL(6, 28, 19, 52), SPL(19, 52, 58, 52),
                        SPL(58, 52, 58, 0), SPL(58, 0, 19, 0),
                        SPL(19, 0, 6, 24), SPL(6, 24, 6, 28)};
const SystPrim bLb[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(6, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(58, 28, 58, 24),
                        SPL(58, 24, 45, 0), SPL(45, 0, 6, 0)};
const SystPrim bLc[] = {SPL(6, 28, 19, 52), SPL(19, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(58, 14, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24),
                        SPL(6, 24, 6, 28)};
const SystPrim bLd[] = {SPL(58, 0, 58, 25.5f), SPL(58, 44.5f, 58, 70), SPL(19, 52, 58, 52),
                        SPL(19, 52, 6, 28), SPL(6, 28, 6, 24),
                        SPL(6, 24, 19, 0), SPL(19, 0, 58, 0)};
const SystPrim bLe[] = {SPL(6, 28, 19, 52), SPL(19, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(6, 28, 58, 28),
                        SPL(6, 24, 6, 28), SPL(19, 0, 6, 24),
                        SPL(19, 0, 45, 0)};
const SystPrim bLf[] = {SPL(26, 70, 42, 70), SPL(15, 58, 26, 70),
                        SPL(15, 0, 15, 58), SPL(7, 38, 35, 38)};
const SystPrim bLg[] = {SPL(6, 28, 19, 52), SPL(19, 52, 58, 52),
                        SPL(58, 52, 58, -6), SPL(58, 0, 19, 0),
                        SPL(19, 0, 6, 24), SPL(6, 24, 6, 28),
                        SPL(58, -6, 50, -20), SPL(36, -20, 50, -20)};
const SystPrim bLh[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(6, 28, 19, 52),
                        SPL(19, 52, 58, 52), SPL(58, 52, 58, 0)};
const SystPrim bLi[] = {SPL(19, 0, 19, 52), SPL(19, 63, 19, 69)};
const SystPrim bLj[] = {SPL(38, -6, 38, 52), SPL(38, -6, 30, -20),
                        SPL(16, -20, 30, -20), SPL(38, 63, 38, 69)};
const SystPrim bLk[] = {SPL(6, 0, 6, 25.5f), SPL(6, 44.5f, 6, 70), SPL(48, 52, 8, 22), SPL(14, 28, 50, 0)};
const SystPrim bLl[] = {SPL(19, 0, 19, 25.5f), SPL(19, 44.5f, 19, 70)};
const SystPrim bLm[] = {SPL(6, 0, 6, 52), SPL(6, 28, 19, 52),
                        SPL(19, 52, 45, 52), SPL(45, 0, 45, 52),
                        SPL(45, 28, 58, 52), SPL(58, 52, 84, 52),
                        SPL(84, 52, 84, 0)};
const SystPrim bLn[] = {SPL(6, 0, 6, 52), SPL(6, 28, 19, 52),
                        SPL(19, 52, 58, 52), SPL(58, 52, 58, 0)};
const SystPrim bLo[] = {SPL(6, 24, 6, 28), SPL(6, 28, 19, 52),
                        SPL(19, 52, 45, 52), SPL(45, 52, 58, 28),
                        SPL(58, 28, 58, 24), SPL(58, 24, 45, 0),
                        SPL(45, 0, 19, 0), SPL(19, 0, 6, 24)};
const SystPrim bLp[] = {SPL(6, -20, 6, 52), SPL(6, 52, 45, 52),
                        SPL(45, 52, 58, 28), SPL(58, 28, 58, 24),
                        SPL(58, 24, 45, 0), SPL(45, 0, 6, 0)};
const SystPrim bLq[] = {SPL(58, -20, 58, 52), SPL(19, 52, 58, 52),
                        SPL(19, 52, 6, 28), SPL(6, 28, 6, 24),
                        SPL(6, 24, 19, 0), SPL(19, 0, 58, 0)};
const SystPrim bLr[] = {SPL(6, 0, 6, 52), SPL(6, 52, 28, 52),
                        SPL(28, 52, 36, 38)};
const SystPrim bLs[] = {SPL(15, 52, 43, 52), SPL(8, 39, 15, 52),
                        SPL(8, 26, 8, 39), SPL(8, 26, 48, 26),
                        SPL(48, 13, 48, 26), SPL(41, 0, 48, 13),
                        SPL(13, 0, 41, 0)};
const SystPrim bLowT[] = {SPL(20, 0, 20, 70), SPL(8, 38, 36, 38),
                          SPL(20, 0, 36, 0)};
const SystPrim bLu[] = {SPL(6, 24, 6, 52), SPL(6, 24, 19, 0),
                        SPL(19, 0, 45, 0), SPL(45, 0, 58, 24),
                        SPL(58, 0, 58, 52)};
const SystPrim bLv[] = {SPL(6, 52, 32, 0), SPL(58, 52, 32, 0)};
const SystPrim bLw[] = {SPL(6, 52, 22, 0), SPL(22, 0, 42, 36), SPL(42, 36, 62, 0),
                        SPL(62, 0, 84, 52)};
const SystPrim bLx[] = {SPL(8, 0, 48, 52), SPL(8, 52, 48, 0)};
const SystPrim bLy[] = {SPL(6, 52, 24, 14), SPL(48, 52, 24, 14), SPL(24, 14, 14, -16)};
const SystPrim bLz[] = {SPL(8, 52, 48, 52), SPL(48, 52, 8, 0), SPL(8, 0, 48, 0)};

const SystGlyphDef kSystBreakGlyphs[] = {
    SystGlyphDef{U' ', 38, nullptr, 0},
    SG(U'!', 36, bBang), SG(U'"', 40, bDQuote), SG(U'#', 70, bHash),
    SG(U'$', 64, bDollar), SG(U'%', 92, bPercent), SG(U'&', 70, bAmp),
    SG(U'\'', 28, bSQuote), SG(U'(', 40, bLParen), SG(U')', 40, bRParen),
    SG(U'*', 52, bStar), SG(U'+', 60, bPlus), SG(U',', 36, bComma),
    SG(U'-', 48, bMinus), SG(U'.', 36, bDot), SG(U'/', 48, bSlash),
    SG(U'0', 72, bD0), SG(U'1', 72, bD1), SG(U'2', 72, bD2), SG(U'3', 72, bD3),
    SG(U'4', 72, bD4), SG(U'5', 72, bD5), SG(U'6', 72, bD6), SG(U'7', 72, bD7),
    SG(U'8', 72, bD8), SG(U'9', 72, bD9),
    SG(U':', 36, bColon), SG(U';', 36, bSemi), SG(U'<', 60, bLt),
    SG(U'=', 56, bEq), SG(U'>', 60, bGt), SG(U'?', 60, bQuest),
    SG(U'@', 84, bAt),
    SG(U'A', 72, bUA), SG(U'B', 68, bUB), SG(U'C', 72, bUC), SG(U'D', 72, bUD),
    SG(U'E', 70, bUE), SG(U'F', 66, bUF), SG(U'G', 72, bUG), SG(U'H', 72, bUH),
    SG(U'I', 46, bUI), SG(U'J', 72, bUJ), SG(U'K', 72, bUK), SG(U'L', 70, bUL),
    SG(U'M', 98, bUM), SG(U'N', 72, bUN), SG(U'O', 72, bUO), SG(U'P', 68, bUP),
    SG(U'Q', 72, bUQ), SG(U'R', 72, bUR), SG(U'S', 68, bUS), SG(U'T', 72, bUT),
    SG(U'U', 72, bUU), SG(U'V', 72, bUV), SG(U'W', 98, bUW), SG(U'X', 72, bUX),
    SG(U'Y', 72, bUY), SG(U'Z', 72, bUZ),
    SG(U'[', 40, bLBrack), SG(U'\\', 48, bBSlash), SG(U']', 40, bRBrack),
    SG(U'^', 52, bCaret), SG(U'_', 56, bUnder), SG(U'`', 32, bGrave),
    SG(U'a', 72, bLa), SG(U'b', 72, bLb), SG(U'c', 72, bLc), SG(U'd', 72, bLd),
    SG(U'e', 72, bLe), SG(U'f', 52, bLf), SG(U'g', 72, bLg), SG(U'h', 72, bLh),
    SG(U'i', 46, bLi), SG(U'j', 52, bLj), SG(U'k', 66, bLk), SG(U'l', 46, bLl),
    SG(U'm', 98, bLm), SG(U'n', 72, bLn), SG(U'o', 72, bLo), SG(U'p', 72, bLp),
    SG(U'q', 72, bLq), SG(U'r', 52, bLr), SG(U's', 60, bLs), SG(U't', 52, bLowT),
    SG(U'u', 72, bLu), SG(U'v', 72, bLv), SG(U'w', 98, bLw), SG(U'x', 64, bLx),
    SG(U'y', 64, bLy), SG(U'z', 64, bLz),
    SG(U'{', 44, bLBrace), SG(U'|', 32, bBar), SG(U'}', 44, bRBrace),
    SG(U'~', 56, bTilde),
};
#undef SG
#undef SPL
#undef SPA

const SystGlyphDef* systTextGlyph(char32_t cp) {
    for (const SystGlyphDef& g : kSystTextGlyphs) {
        if (g.cp == cp) return &g;
    }
    return nullptr;
}

void systTextPrims(char32_t cp, std::vector<SystPrim>& out) {
    const SystGlyphDef* g = systTextGlyph(cp);
    if (!g) return;
    for (int i = 0; i < g->count; ++i) out.push_back(g->prims[i]);
}

const SystGlyphDef* systBreakGlyph(char32_t cp) {
    for (const SystGlyphDef& g : kSystBreakGlyphs) {
        if (g.cp == cp) return &g;
    }
    return nullptr;
}

void systBreakPrims(char32_t cp, std::vector<SystPrim>& out) {
    const SystGlyphDef* g = systBreakGlyph(cp);
    if (!g) return;
    for (int i = 0; i < g->count; ++i) out.push_back(g->prims[i]);
}

// Geometry over prims: exact distances (segments as in the line
// faces; arcs by radial distance inside the sweep, endpoint
// distance outside it), so the distance-field bake stays smooth.
constexpr float kPi = 3.14159265358979323846f;

Vec2 arcPointAt(const SystPrim& pr, float deg) {
    const float rad = deg * kPi / 180.0f;
    return {pr.c.x + pr.r * std::cos(rad), pr.c.y + pr.r * std::sin(rad)};
}

// Is `ang` (degrees, any range) covered by the arc's sweep?
bool angleInSweep(float ang, float a0, float sweep) {
    if (sweep >= 360.0f || sweep <= -360.0f) return true;
    float rel = std::fmod(ang - a0, 360.0f);
    if (rel < 0.0f) rel += 360.0f;
    return sweep >= 0.0f ? (rel <= sweep) : (rel >= 360.0f + sweep);
}

float primPointDist(Vec2 p, const SystPrim& pr) {
    if (!pr.arc) return segPointDist(p, pr.a, pr.b);
    const float dx = p.x - pr.c.x, dy = p.y - pr.c.y;
    const float dc = std::sqrt(dx * dx + dy * dy);
    float ang = std::atan2(dy, dx) * 180.0f / kPi;
    if (ang < 0.0f) ang += 360.0f;
    if (angleInSweep(ang, pr.a0, pr.sweep)) return std::fabs(dc - pr.r);
    const Vec2 e0 = arcPointAt(pr, pr.a0);
    const Vec2 e1 = arcPointAt(pr, pr.a0 + pr.sweep);
    const float d0x = p.x - e0.x, d0y = p.y - e0.y;
    const float d1x = p.x - e1.x, d1y = p.y - e1.y;
    return std::min(std::sqrt(d0x * d0x + d0y * d0y),
                    std::sqrt(d1x * d1x + d1y * d1y));
}

// Grow the bounds (minX, minY, maxX, maxY) by one prim's true
// extent: segment endpoints; arc endpoints plus the cardinal
// extremes the sweep passes.
void primBounds(const SystPrim& pr, float& minX, float& minY, float& maxX, float& maxY) {
    auto grow = [&](Vec2 p) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    };
    if (!pr.arc) {
        grow(pr.a);
        grow(pr.b);
        return;
    }
    grow(arcPointAt(pr, pr.a0));
    grow(arcPointAt(pr, pr.a0 + pr.sweep));
    const float cardinals[4] = {0.0f, 90.0f, 180.0f, 270.0f};
    for (float card : cardinals)
        if (angleInSweep(card, pr.a0, pr.sweep)) grow(arcPointAt(pr, card));
}

// UTF-8 decode (same contract as the GUI core: malformed = U+FFFD,
// and overlong encodings, surrogates and values above U+10FFFF in
// well-formed sequences also normalize to U+FFFD, review CORE-M10).
char32_t nextCp(const std::string& s, std::size_t& i) {
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

constexpr float kUnitsPerEm = 100.0f;
constexpr float kSystAdvance = 64.0f;
// Cell mapping: font point (gx, gy) lands at atlas pixel
// (cellX + kCellPad + gx, cellY + kCellBaseline - gy) at bake scale
// 1 unit = 1 px.
constexpr float kCellPad = 4.0f;
constexpr float kCellBaseline = 84.0f;

} // namespace

// ------------------------------------------------------------- provider

RasterTextMetrics::RasterTextMetrics() {
    atlas_.width = kCols * kCellW;
    atlas_.height = (kMaxCells / kCols) * kCellH;
    atlas_.coverage.assign(static_cast<std::size_t>(atlas_.width) *
                               static_cast<std::size_t>(atlas_.height),
                           0);
}

void RasterTextMetrics::bindFace(FontId id, BuiltinFace face) {
    bindings_[id] = face;
}

bool RasterTextMetrics::faceKnown(FontId id, BuiltinFace& out) const {
    auto it = bindings_.find(id);
    if (it != bindings_.end()) { out = it->second; return true; }
    if (id == FONT_DEFAULT) { out = BuiltinFace::Latin; return true; }
    return false;
}

bool RasterTextMetrics::hasGlyph(FontId font, char32_t cp) const {
    BuiltinFace face;
    if (!faceKnown(font, face)) return false;
    if (face == BuiltinFace::Systerian) return systerianValue(cp) != 0;
    if (face == BuiltinFace::SysterianText) return systTextGlyph(cp) != nullptr;
    if (face == BuiltinFace::SysterianBreak) return systBreakGlyph(cp) != nullptr;
    // Latin and SumerianWedge: the same printable-ASCII coverage,
    // by construction (the wedge face derives from the Latin table).
    return cp >= 0x20 && cp <= 0x7E;
}

float RasterTextMetrics::glyphAdvance(char32_t cp, FontId font, float size, int) const {
    BuiltinFace face;
    if (!faceKnown(font, face)) return size * 0.6f; // unbound: approximate
    float advUnits;
    if (face == BuiltinFace::Systerian) {
        advUnits = kSystAdvance; // numerals and .notdef alike: one cell
    } else if (face == BuiltinFace::SysterianText ||
               face == BuiltinFace::SysterianBreak) {
        // Advances derive from the hand-authored constructions:
        // each is the glyph's authored span plus its bearing;
        // the bearing was widened to +20 total by JD's spacing
        // correction (2026-11-02; see the faces' section notes).
        // The Break face shares the rule: identical advances,
        // except its rebuilt S and s (wider spans).
        const SystGlyphDef* g = face == BuiltinFace::SysterianText
                                    ? systTextGlyph(cp)
                                    : systBreakGlyph(cp);
        if (g) advUnits = static_cast<float>(g->adv);
        else advUnits = 52.0f; // .notdef: the Latin '?' advance
    } else {
        // Latin and SumerianWedge share the skeleton, so they share
        // advances exactly (asserted in the smoke suite).
        if (const GlyphDef* g = latinGlyph(cp)) advUnits = static_cast<float>(g->adv);
        else advUnits = 52.0f; // .notdef: the '?' advance
    }
    return advUnits * size / kUnitsPerEm;
}

float RasterTextMetrics::measure(const std::string& text, const TextStyle& style) const {
    // Segment through the fallback chain first (review SUP-3): a code
    // point the primary font lacks is measured with the RESOLVED
    // font's advance, exactly as resolveRuns segments it for drawing,
    // not with the primary font's .notdef advance.
    float total = 0.0f;
    std::size_t count = 0;
    for (const ResolvedRun& run : resolveRuns(text, style, *this)) {
        std::size_t i = 0;
        while (i < run.text.size()) {
            char32_t cp = nextCp(run.text, i);
            total += glyphAdvance(cp, run.font, style.size, style.weight);
            ++count;
        }
    }
    if (count > 1) total += style.letterSpacing * static_cast<float>(count - 1);
    return total;
}

bool RasterTextMetrics::glyphAvailable(char32_t cp, FontId font, float, int) const {
    return hasGlyph(font, cp);
}

bool RasterTextMetrics::glyphNeedsNewCell(char32_t cp, FontId font, float, int weight) const {
    BuiltinFace face;
    if (!faceKnown(font, face)) return false;
    if (!hasGlyph(font, cp)) return false;
    CellKey key{static_cast<int>(face), cp, weight >= 600};
    return cells_.find(key) == cells_.end();
}

FontMetrics RasterTextMetrics::fontMetrics(FontId font, float size) const {
    BuiltinFace face;
    FontMetrics m;
    if (faceKnown(font, face) && face == BuiltinFace::Systerian) {
        m = {66.0f, 42.0f, 14.0f};
    } else {
        m = {76.0f, 22.0f, 24.0f}; // Latin metrics: the text display faces and the unbound default too
    }
    const float s = size / kUnitsPerEm;
    return {m.ascent * s, m.descent * s, m.lineGap * s};
}

int RasterTextMetrics::cellFor(BuiltinFace face, char32_t cp, bool bold) const {
    CellKey key{static_cast<int>(face), cp, bold};
    auto it = cells_.find(key);
    if (it != cells_.end()) return it->second;
    if (nextCell_ >= kMaxCells) return -1; // atlas full: honest miss
    int cell = nextCell_++;
    cells_[key] = cell;
    bakeCell(cell, face, cp, bold);
    return cell;
}

void RasterTextMetrics::bakeCell(int cell, BuiltinFace face, char32_t cp, bool bold) const {
    std::vector<Seg> segs;
    std::vector<SystPrim> prims;
    if (face == BuiltinFace::Systerian) systSegments(systerianValue(cp), segs);
    else if (face == BuiltinFace::SysterianText) systTextPrims(cp, prims);
    else if (face == BuiltinFace::SysterianBreak) systBreakPrims(cp, prims);
    else latinSegments(cp, segs); // Latin and SumerianWedge: one skeleton
    if (segs.empty() && prims.empty()) return; // space and friends: a blank cell
    const float radius = bold ? 6.5f : 4.5f;
    // SumerianWedge bakes the Latin segments as filled wedge
    // triangles instead of stroked lines (see wedgeTriangles).
    const bool wedges = face == BuiltinFace::SumerianWedge;
    std::vector<WedgeTri> tris;
    float pad = radius; // how far ink can reach past a skeleton point
    if (wedges) {
        const float w = 2.0f * radius; // nominal stroke width W
        wedgeTriangles(segs, 1.8f * w, 0.8f * w, tris); // max length, head half-width
        pad = 0.8f * w;
    }
    const int cellX = (cell % kCols) * kCellW;
    const int cellY = (cell / kCols) * kCellH;
    // Ink bounds in cell pixel space, padded by the ink reach.
    float minFx = 1.0e9f, minFy = 1.0e9f, maxFx = -1.0e9f, maxFy = -1.0e9f;
    for (const Seg& s : segs) {
        minFx = std::min({minFx, s.first.x, s.second.x});
        maxFx = std::max({maxFx, s.first.x, s.second.x});
        minFy = std::min({minFy, s.first.y, s.second.y});
        maxFy = std::max({maxFy, s.first.y, s.second.y});
    }
    for (const SystPrim& pr : prims) primBounds(pr, minFx, minFy, maxFx, maxFy);
    const int x0 = std::max(cellX, static_cast<int>(std::floor(
                                       cellX + kCellPad + minFx - pad - 1.0f)));
    const int x1 = std::min(cellX + kCellW, static_cast<int>(std::ceil(
                                                cellX + kCellPad + maxFx + pad + 1.0f)));
    const int y0 = std::max(cellY, static_cast<int>(std::floor(
                                       cellY + kCellBaseline - maxFy - pad - 1.0f)));
    const int y1 = std::min(cellY + kCellH, static_cast<int>(std::ceil(
                                                cellY + kCellBaseline - minFy + pad + 1.0f)));
    // Distance-based coverage, 2x2 supersampled: each sub-sample's
    // coverage is clamp(radius + 0.5 - distance to the nearest
    // stroke) for the line faces (segments for Latin and the
    // numerals, segments and arcs for Systerian Text),
    // clamp(0.5 - signed distance to the nearest wedge) for
    // SumerianWedge; the four sub-samples are averaged into the
    // texel.
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            float cov = 0.0f;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    const float ax = px + 0.25f + 0.5f * static_cast<float>(sx);
                    const float ay = py + 0.25f + 0.5f * static_cast<float>(sy);
                    const Vec2 fp{ax - cellX - kCellPad,
                                  kCellBaseline - (ay - cellY)};
                    float d = 1.0e9f;
                    if (wedges) {
                        for (const WedgeTri& t : tris) d = std::min(d, triPointDist(fp, t));
                        cov += std::clamp(0.5f - d, 0.0f, 1.0f);
                    } else {
                        for (const Seg& s : segs) d = std::min(d, segPointDist(fp, s.first, s.second));
                        for (const SystPrim& pr : prims) d = std::min(d, primPointDist(fp, pr));
                        cov += std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
                    }
                }
            }
            cov *= 0.25f;
            if (cov > 0.0f) {
                atlas_.coverage[static_cast<std::size_t>(py) *
                                    static_cast<std::size_t>(atlas_.width) +
                                static_cast<std::size_t>(px)] =
                    static_cast<std::uint8_t>(cov * 255.0f + 0.5f);
            }
        }
    }
}

bool RasterTextMetrics::glyphQuad(char32_t cp, FontId font, float size, int weight,
                                  float penX, float baselineY,
                                  Rect& outDest, Rect& outUv, int& outAtlas) const {
    BuiltinFace face;
    if (!faceKnown(font, face)) return false;
    if (!hasGlyph(font, cp)) return false;
    const int cell = cellFor(face, cp, weight >= 600);
    if (cell < 0) return false;
    const float s = size / kUnitsPerEm;
    // The destination is the whole cell at the pen: a glyph point
    // (gx, gy) lands at dest offset (kCellPad + gx, kCellBaseline -
    // gy) * s, which is exactly where bakeCell put it in the atlas.
    outDest = {penX - kCellPad * s, baselineY - kCellBaseline * s,
               static_cast<float>(kCellW) * s, static_cast<float>(kCellH) * s};
    const int cx = (cell % kCols) * kCellW;
    const int cy = (cell / kCols) * kCellH;
    outUv = {static_cast<float>(cx) / static_cast<float>(atlas_.width),
             static_cast<float>(cy) / static_cast<float>(atlas_.height),
             static_cast<float>(kCellW) / static_cast<float>(atlas_.width),
             static_cast<float>(kCellH) / static_cast<float>(atlas_.height)};
    outAtlas = kAtlasId;
    return true;
}

} // namespace lithogui
