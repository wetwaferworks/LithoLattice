// Litho GUI - in-tree reference font layer (2026-11-01).
//
// The base layer defines font CONTRACTS (FontRegistry, ITextMetrics,
// TextRun); this module is the first concrete implementation behind
// them, written in-house like everything else in the tree: no font
// files, no font libraries. Glyph shapes are stroke (vector) data:
// printable ASCII is authored as polylines in the tables in
// litho_font.cpp, and Systerian (Cistercian) numerals are generated
// procedurally from their stroke construction. A third face,
// Sumerian, reuses the Latin tables unchanged but bakes every
// stroke as a chain of filled cuneiform wedge impressions, so the
// letterforms stay English-readable under a Sumerian carved look.
// A fourth face, Systerian Text, is hand-authored in the Systerian
// numerals' own construction language (straight segments only, all
// diagonals at the numeral quadrant's stave angle, bowls as
// chamfered frames), so the alphabet and the numerals form one
// script family: sharp, geometric, and English-readable. A software rasterizer
// bakes glyphs on demand into a grayscale coverage atlas
// (distance-based coverage, 2x2 supersampled), and the same tables
// drive the metrics, so measure() always agrees with the atlas
// advances by construction.
//
// The provider implements BOTH seams: ITextMetrics (layout) and
// IGlyphSource (pixel output). Bind the same instance as the
// context's metrics and glyph source (UiContext::setMetrics +
// setGlyphSource) and layout, segmentation and rasterization can
// never disagree.
//
// SYSTERIAN CODEPOINT MAPPING (this layer's assignment, stable):
// Cistercian numerals have no Unicode block, and the values 1..9999
// do not fit the BMP Private Use Area (U+E000..U+F8FF is 6,400
// slots), so Systerian values live in Supplementary Private Use
// Area A: code point = 0xF0000 + value, i.e. U+F0001 (1) through
// U+F270F (9999). There is no glyph for value 0: Cistercian has no
// zero; the stave alone is never a numeral. A font face bound to
// BuiltinFace::Systerian covers exactly this range, so a TextStyle
// whose fallback chain ends at a Systerian face resolves numerals
// there through the ordinary resolveRuns segmentation, and text
// carrying these code points renders as true Cistercian glyphs.
#pragma once

#include "litholattice/ui/litho_gui.h"

#include <cstdint>
#include <map>
#include <vector>

namespace lithogui {

// ------------------------------------------------------- Systerian map
constexpr char32_t SYSTERIAN_BASE = 0xF0000; // Supplementary PUA-A
constexpr char32_t systerianCodePoint(int value) {
    return SYSTERIAN_BASE + static_cast<char32_t>(value); // value: 1..9999
}
constexpr int systerianValue(char32_t cp) {
    return (cp > SYSTERIAN_BASE && cp <= SYSTERIAN_BASE + 9999)
               ? static_cast<int>(cp - SYSTERIAN_BASE)
               : 0;
}

// The built-in faces this provider can serve. A FontId is bound
// to a face explicitly (bindFace); unbound FONT_DEFAULT serves as
// Latin, other unbound fonts have no glyphs at all.
//   Latin:         the authored stroke skeleton, line-baked.
//   Systerian:     Cistercian numerals 1..9999 (PUA-A codepoints).
//   SumerianWedge: the Latin skeleton wedge-baked - every stroke
//                  rendered as cuneiform wedge impressions (a
//                  Sumerian display face over English letterforms).
//                  Built 2026-11-02 under the name "Systerian
//                  Text" from a transcription error (JD said
//                  Systerian; the transcript wrote Sumerian) and
//                  renamed to its honest name the same night; the
//                  face itself is unchanged.
//   SysterianText: Latin letterforms hand-authored in the
//                  Systerian numerals' construction language -
//                  straight segments only: stave-angle
//                  diagonals and chamfered-frame bowls,
//                  monoline line-baked, sharp and geometric
//                  (JD's design, angular revision 2026-11-02:
//                  less roundedness, sharp angles at the
//                  stave angle; JD speaks the name as
//                  "Lithoglyphs", the transcription-safe
//                  alias).
//   SysterianBreak: the readable variation of Systerian Text
//                  (JD's ruling, 2026-11-02): the angular
//                  constructions carried over, but the S is
//                  the instantly readable segmented form and
//                  the primary stems of capitals, digits and
//                  the lowercase ascenders carry one clean
//                  break at the quadrant seam (y = 35) - the
//                  broken-stave signature. (Scope corrected in
//                  round-3 review FNT-3: descender stems -
//                  p, q, g, j - cross the seam unsplit, and
//                  bar/diagonal-joined stems stay whole; the
//                  exact rule lives with the tables in
//                  litho_font.cpp.) Readability wins; the
//                  angular face stays as the "hard"
//                  variation.
enum class BuiltinFace { Latin, Systerian, SumerianWedge, SysterianText, SysterianBreak };

// The baked coverage atlas: one grayscale image, 8 bits per texel.
// Texel (x, y) is coverage[y * width + x]; 0 = no ink, 255 = full.
struct GlyphAtlasImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> coverage;
    std::uint8_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(x)];
    }
};

// The reference font provider: authored Latin stroke font +
// procedural Systerian numerals + the Sumerian wedge face over the
// Latin skeleton + the hand-authored Systerian Text face + its
// readable Systerian Break variation, one shared coverage atlas.
//
// Font-unit space: 100 units per em. Latin: baseline at 0, cap
// height 70, ascent 76, descent 22, line gap 24. Systerian: the stave
// spans y -36..60; ascent 66, descent 42, line gap 14 (both faces'
// natural line heights sum to 122 units, so mixed lines align).
// The text display faces (Sumerian, Systerian Text, Systerian
// Break) share the Latin em frame and metrics, so they mix on
// Latin lines.
// Weight >= 600 bakes a bolder stroke (a separate atlas cell);
// advances never depend on weight. Slant is not synthesized: slanted
// styles measure and rasterize as the upright face (documented in
// README).
class RasterTextMetrics : public ITextMetrics, public IGlyphSource {
public:
    // Atlas geometry (pixels). Cells are fixed-size so a glyph's UV
    // never moves once baked; the image is allocated at full size up
    // front so normalized UVs are stable for the provider's life.
    static constexpr int kCellW = 96;
    static constexpr int kCellH = 128;
    static constexpr int kCols = 16;
    static constexpr int kMaxCells = 256; // 16 x 16 cells, 1536x2048 atlas
    static constexpr int kAtlasId = 0;

    RasterTextMetrics();

    void bindFace(FontId id, BuiltinFace face);

    // ITextMetrics. measure() is exactly the sum of glyphAdvance()
    // over the text's code points plus letter spacing, so metrics
    // self-consistency holds by construction.
    float measure(const std::string& text, const TextStyle& style) const override;
    FontMetrics fontMetrics(FontId font, float size) const override;
    float glyphAdvance(char32_t cp, FontId font, float size, int weight) const override;
    bool hasGlyph(FontId font, char32_t cp) const override;

    // IGlyphSource. Bakes the glyph into the atlas on first use.
    bool glyphQuad(char32_t cp, FontId font, float size, int weight,
                   float penX, float baselineY,
                   Rect& outDest, Rect& outUv, int& outAtlas) const override;
    // Non-baking availability queries (see IGlyphSource).
    bool glyphAvailable(char32_t cp, FontId font, float size, int weight) const override;
    bool glyphNeedsNewCell(char32_t cp, FontId font, float size, int weight) const override;
    int freeGlyphCells() const override { return kMaxCells - nextCell_; }

    const GlyphAtlasImage& atlasImage() const { return atlas_; }
    int bakedGlyphCount() const { return nextCell_; }

private:
    bool faceKnown(FontId id, BuiltinFace& out) const;
    // Cell index for (face, cp, bold), baking on first use; -1 when
    // the atlas is full (the run then stays a TextRun, honestly).
    int cellFor(BuiltinFace face, char32_t cp, bool bold) const;
    void bakeCell(int cell, BuiltinFace face, char32_t cp, bool bold) const;

    std::map<FontId, BuiltinFace> bindings_; // ordered: deterministic
    mutable GlyphAtlasImage atlas_;
    struct CellKey {
        int face;
        char32_t cp;
        bool bold;
        bool operator<(const CellKey& o) const {
            if (face != o.face) return face < o.face;
            if (cp != o.cp) return cp < o.cp;
            return bold < o.bold;
        }
    };
    mutable std::map<CellKey, int> cells_;
    mutable int nextCell_ = 0;
};

} // namespace lithogui
