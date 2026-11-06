// Litho GUI - base layer for the LithoLattice engine.
// Original in-house code, C++17, no third-party dependencies.
//
// Architecture: a retained widget tree (documents, focus, tree views are
// natural in retained mode) that emits a renderer-agnostic draw list each
// frame. Vulkan, GL33 and the software back end each consume the same
// command stream. The GUI reads application state and emits input intents
// through callbacks only; it never touches simulation state, never reads a
// wall clock (animation phase is passed in per frame), and never iterates a
// hash map into behavior or draw order: children live in vectors and are
// always walked in insertion order.
//
// Engine integration (2026-11-06): this is the Litho GUI subsystem at its
// own version — Litho GUI versionName 0.1, versionCode 27 (Build 27:
// universal Frame inheritance, 29 widget classes). The GUI subsystem
// version is deliberately separate from the LithoLattice engine build
// counter; never conflate the two. See src/ui/LITHO-GUI-INTEGRATION.md.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lithogui {

// ---------------------------------------------------------------- geometry

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    bool contains(Vec2 p) const { return contains(p.x, p.y); }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

struct Edges {
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    Edges() = default;
    explicit Edges(float all) : l(all), t(all), r(all), b(all) {}
    Edges(float lr, float tb) : l(lr), t(tb), r(lr), b(tb) {}
};

// ------------------------------------------------------------------- fonts
// Custom font support is a contract, not an implementation: the base layer
// registers font sources and measures text through a provider interface.
// Loading, atlasing, shaping and rasterization live in the engine's font
// layer behind ITextMetrics; no font library is imported here.

using FontId = int;
constexpr FontId FONT_INVALID = -1;
constexpr FontId FONT_DEFAULT = 0;

enum class FontSlant { Normal, Italic, Oblique };

struct FontSource {
    enum class Kind { FilePath, Memory, BitmapFont };
    Kind kind = Kind::FilePath;
    std::string path;             // FilePath: font file to load
    const std::uint8_t* bytes = nullptr; // Memory: caller-owned font bytes
    std::size_t byteCount = 0;
    std::string bitmapDescriptor; // BitmapFont: engine bitmap-font descriptor
};

struct FontFace {
    FontId id = FONT_INVALID; // registry assigns when left FONT_INVALID
    std::string name;         // stable display/lookup name, e.g. "Litho Sans"
    std::string family;       // e.g. "Litho Sans", "Systerian Symbols"
    int weight = 400;         // 100..900
    FontSlant slant = FontSlant::Normal;
    FontSource source;
};

// Registry of the application's custom fonts. Faces are stored in
// registration order; lookups are linear and deterministic.
class FontRegistry {
public:
    // Registers a face; assigns an id when face.id is FONT_INVALID.
    // Returns the face's id.
    FontId registerFont(FontFace face);
    const FontFace* find(FontId id) const;
    // Nearest face in a family: exact weight/slant when present, otherwise
    // the first registered face of that family.
    const FontFace* find(const std::string& family, int weight, FontSlant slant) const;
    const std::vector<FontFace>& faces() const { return faces_; }

private:
    std::vector<FontFace> faces_;
    FontId nextId_ = 1;
};

enum class TextAlign { Left, Center, Right };
enum class TextWrap { None, Word };
enum class Ellipsis { None, End };

// A complete text style: which font (plus its fallback chain), how big,
// how heavy, how spaced, and how it breaks. The fallback chain is ordered:
// the first font in {font} + fallbacks that has a glyph for a code point
// renders that code point. Typical chains: UI font -> icon font ->
// Systerian symbol font.
struct TextStyle {
    FontId font = FONT_DEFAULT;
    float size = 14.0f;
    int weight = 400;
    FontSlant slant = FontSlant::Normal;
    Color color{0, 0, 0, 0}; // alpha 0 = inherit the Style palette text color
    float letterSpacing = 0.0f; // extra advance per character, px
    float lineHeightMul = 1.2f; // multiplier over the font's natural line height
    TextAlign align = TextAlign::Left;
    TextWrap wrap = TextWrap::None;
    Ellipsis ellipsis = Ellipsis::None;
    // Decorations drawn by Label (and the VirtualList rich rows) as
    // line commands under / through the run, in the run's color.
    // Added for markdown links (underline) and ~~strike~~ spans.
    bool underline = false;
    bool strikethrough = false;
    std::vector<FontId> fallbacks;
};

// One rich-text run: a piece of text with its own style. A nullopt style
// inherits the base style of the string it belongs to.
struct TextSpan {
    std::string text;
    std::optional<TextStyle> style;
};

struct FontMetrics {
    float ascent = 0.0f;   // baseline to top, px at the requested size
    float descent = 0.0f;  // baseline to bottom, positive value
    float lineGap = 0.0f;  // extra leading between lines
};

// ------------------------------------------------------------ text metrics
// The font provider contract. The engine's font layer implements this over
// its own loader/atlas; the GUI uses it for layout, caret placement and
// fallback resolution only.
class ITextMetrics {
public:
    virtual ~ITextMetrics() = default;
    virtual float measure(const std::string& text, const TextStyle& style) const = 0;
    virtual FontMetrics fontMetrics(FontId font, float size) const = 0;
    virtual float glyphAdvance(char32_t cp, FontId font, float size, int weight) const = 0;
    virtual bool hasGlyph(FontId font, char32_t cp) const = 0;
    float lineHeight(const TextStyle& style) const {
        FontMetrics m = fontMetrics(style.font, style.size);
        return (m.ascent + m.descent + m.lineGap) * style.lineHeightMul;
    }
};

// A text run after fallback resolution: a maximal run of code points that
// one font renders. resolveRuns is the segmentation contract: the GUI uses
// it where a widget must know run boundaries, and a back end consuming a
// TextRun whose TextStyle carries fallbacks performs the same segmentation
// with the same provider before rasterizing.
struct ResolvedRun {
    std::string text; // UTF-8 bytes of the run
    FontId font = FONT_DEFAULT;
};
std::vector<ResolvedRun> resolveRuns(const std::string& text, const TextStyle& style,
                                     const ITextMetrics& metrics);

// Fallback metrics used when the host provides none: fixed advance of
// 0.6 * size per byte, every glyph present. Good enough for layout smoke
// tests, not for shipping text.
class ApproxTextMetrics : public ITextMetrics {
public:
    float measure(const std::string& text, const TextStyle& style) const override;
    FontMetrics fontMetrics(FontId, float size) const override;
    float glyphAdvance(char32_t, FontId, float size, int) const override {
        return size * 0.6f;
    }
    bool hasGlyph(FontId, char32_t) const override { return true; }
};

// ------------------------------------------------------------------- style

// Palette and metrics tokens. tracePhase is the ONLY time-like value in the
// system and it is supplied by the caller once per frame; GUI code never
// reads a clock itself.
//
// Opacity model (the overlay is user-adjustable):
//   effective fill alpha = color.a * uiOpacity * (per-widget opacity
//   override chain, see Widget::opacityOverride).
// Trace borders ignore the fill opacity entirely and use traceOpacity, so
// panels can go nearly clear while the circuit borders stay readable.
struct Style {
    Color windowBg{0.08f, 0.09f, 0.13f, 1.0f};
    Color titleBar{0.12f, 0.14f, 0.22f, 1.0f};
    Color panelBg{0.10f, 0.11f, 0.17f, 1.0f};
    Color controlBg{0.16f, 0.18f, 0.27f, 1.0f};
    Color controlBgHot{0.22f, 0.25f, 0.37f, 1.0f};
    Color selection{0.20f, 0.35f, 0.65f, 0.55f};
    Color text{0.92f, 0.94f, 1.00f, 1.0f};
    Color textMuted{0.55f, 0.60f, 0.72f, 1.0f};
    Color border{0.35f, 0.40f, 0.55f, 1.0f};
    // The Trace signature colors.
    Color traceRed{1.00f, 0.16f, 0.22f, 1.0f};
    Color traceBlue{0.15f, 0.45f, 1.00f, 1.0f};
    Color tracePurple{0.62f, 0.30f, 1.00f, 1.0f};
    // A fully transparent token: assign it to a root background to get a
    // root that emits no background fill (overlay composited over the 3D
    // scene).
    Color clear{0.0f, 0.0f, 0.0f, 0.0f};

    float borderWidth = 1.0f;
    float cornerRadius = 4.0f;
    float titleBarHeight = 28.0f;
    float rowHeight = 22.0f;
    float defaultPadding = 8.0f;
    TextStyle bodyText;  // default font 0, size 14
    TextStyle titleText; // default font 0, size 15 (set in litho_gui.cpp)
    float tracePhase = 0.0f; // caller-supplied animation phase
    float tracePhaseSpeed = 1.0f; // shell advances phase by speed * dt

    float uiOpacity = 1.0f;     // global overlay opacity, user-adjustable
    float windowOpacity = 1.0f; // default for windows without an override
    float traceOpacity = 1.0f;  // trace borders' own opacity
    // Resolution independence: the UI scale factor. The shell speaks
    // PHYSICAL pixels at the ILithoGui / UiContext boundary (layout
    // viewport, pointer coordinates, hit-test queries); the widget tree
    // lays out and hit-tests in LOGICAL units = physical / uiScale; the
    // emitted draw list is scaled back to physical pixels before it
    // leaves emitDraw / emitFrame, so back ends never see logical
    // units. 1.0 means logical units are pixels. User-adjustable like
    // the opacities above (a settings slider in the shell); a theme may
    // also bind it (top-level "uiScale" token). Values <= 0 or
    // non-finite are treated as 1.0. Theme metric tokens (rowHeight,
    // borderWidth, text sizes) are LOGICAL units: they do not change
    // with the scale, they are scaled on output with everything else.
    float uiScale = 1.0f;
    // Debug aid (JD, 2026-11-02): when true, every Frame widget
    // additionally draws loud outlines of its own bounding box
    // and of its inner scaled content box, over its content, so
    // frame bounding boxes can be checked on a device. Purely a
    // draw-time read inside Frame::draw: with the flag off, a
    // Frame's draw list is exactly what it would be without the
    // feature. It is deliberately NOT a theme token: a debug
    // switch belongs to the shell, not to a theme.
    bool debugShowFrameOutlines = false;
    Style();
};

// Shared default style, used as the default argument where a caller has no
// style at hand (widget drag re-layout, tests).
const Style& defaultStyle();

// Style validation at the UiContext::setStyle choke point (round-2
// review C6): returns a copy of `s` in which every metric a division
// or a float->int cast consumes is made safe, mirroring what
// scaleFactor() does for uiScale and what applyThemeJson does for
// theme-supplied values. Non-finite or non-positive rowHeight /
// titleBarHeight / text sizes / lineHeightMul fall back to the
// defaults; negative borderWidth / cornerRadius / defaultPadding
// clamp to 0; opacities clamp to [0, 1]; a non-finite letterSpacing
// becomes 0 and a non-finite tracePhase becomes 0. uiScale is left
// alone (scaleFactor guards every use). A Style set directly through
// setStyle therefore cannot put UB into a live widget the way a
// hostile theme could before SUP-1.
Style validatedStyle(Style s);

// ------------------------------------------------------------------ events
// Input arrives as plain structs. The host translates its platform events
// into these; the GUI translates handled input into intents via callbacks.
// Intents are meant to be queued by the application and, where they affect
// the simulation, quantized to ticks by the application, never by the GUI.

struct PointerEvent {
    enum class Type { Move, Down, Up, Wheel };
    Type type = Type::Move;
    float x = 0.0f;
    float y = 0.0f;
    int button = 0;          // 0 = primary
    float wheelDelta = 0.0f; // +1 = one notch up
    // Host-reported click multiplicity for Down events (1 = single,
    // 2 = double click). The GUI never reads a clock, so the host
    // owns double-click timing and reports the count, the way real
    // toolkits hand it over.
    int clickCount = 1;
    // Modifier state at the time of the event, as the host reports
    // it (build 21: the inventory's shift-click fast transfer and
    // Ctrl forms need to tell a modified press from a plain one
    // through the normal dispatch; before this, PointerEvent
    // carried no modifiers and a shift-click was indistinguishable
    // from a click). The host owns the tracking, exactly as it
    // does for KeyEvent's modifier fields.
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

// Key codes owned by this layer (host maps its native codes onto these).
enum KeyCode : int {
    KEY_NONE = 0,
    KEY_LEFT = 1,
    KEY_RIGHT = 2,
    KEY_BACKSPACE = 3,
    KEY_DELETE = 4,
    KEY_HOME = 5,
    KEY_END = 6,
    KEY_ENTER = 7,
    KEY_TAB = 8,
    KEY_ESCAPE = 9,
    KEY_UP = 10,
    KEY_DOWN = 11,
};

struct KeyEvent {
    int key = KEY_NONE;
    char32_t ch = 0; // printable character input, 0 when none
    bool down = true;
    // Modifier state at the time of the event, as the host reports
    // it. `shift` extends selections (TextField arrows/Home/End and
    // pointer drags). `ctrl` forms the TextField's clipboard chords:
    // ctrl+C/X/V/A arrive as `ch` with ctrl set and the field turns
    // them into clipboard INTENTS (the shell owns the clipboard; see
    // TextField). Other shortcuts remain the shell's own business.
    // `alt` (added 2026-11-02 for the inventory screen): the host
    // maps its Alt modifier here; Alt+digit forms the inventory's
    // extended-slot selection (see src/inventory_screen.h). Digit
    // keys have no KeyCode of their own: a number key arrives as
    // its printable character in `ch` ('1'..'8'), with `alt` saying
    // which bank it addresses.
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

// --------------------------------------------------------------- draw list
// The render contract. A back end walks the command vector in order and
// translates each command: geometry + texture + clip maps directly to
// Vulkan and GL33; the software back end interprets the same stream with
// its rasterizer. Commands are emitted depth-first in widget insertion
// order, so draw order is stable and testable. Popups emit after the whole
// tree, in popup stack order, so they render above it.

struct DrawCmd {
    enum class Kind {
        PushClip,   // rect becomes the active clip (intersected)
        PopClip,
        FillRect,   // rect + color (cornerRadius carried for SDF back ends)
        Line,       // points[0] -> points[1], color, thickness
        Polyline,   // points in order, color, thickness (trace borders)
        TextRun,    // see text fields below
        ImageQuad,  // rect + textureId
        CustomDraw, // tag + payload; back end attaches custom passes
        GlyphQuad,  // rect (destination) + uvRect (atlas) + textureId (atlas)
    };
    Kind kind = Kind::FillRect;
    Rect rect{};
    Color color{};
    float cornerRadius = 0.0f;
    float thickness = 1.0f;
    std::vector<Vec2> points;
    // TextRun payload. rect.x/rect.y is the top-left origin of the run;
    // `baseline` is the distance from rect.y down to the baseline.
    std::string text;
    FontId fontId = FONT_DEFAULT;
    float fontSize = 0.0f;
    int fontWeight = 400;
    int fontSlant = 0; // FontSlant as int
    float letterSpacing = 0.0f;
    float baseline = 0.0f;
    std::vector<FontId> fallbacks; // ordered fallback chain for segmentation
    int textureId = -1;
    // ImageQuad payload: the command's own alpha. The back end
    // MULTIPLIES the texture's texel alpha by this value (blending
    // is straight-alpha source-over); Image sets it from the
    // widget's effective opacity so images fade with windows and
    // themes exactly like fills and text. Dimensionless, like the
    // color channels: DrawList::scale passes it through unchanged.
    // BOUND (round-2 review T-4): imageAlpha is clamped to [0, 1]
    // at emission in DrawList::imageQuad, so a shell that sets an
    // opacityOverride above 1 cannot push a command alpha past 1.
    // GlyphQuad does NOT use it (atlas coverage is in the texture).
    float imageAlpha = 1.0f;
    // GlyphQuad payload: the glyph's sub-rectangle in its atlas, in
    // NORMALIZED atlas coordinates (0..1 across the atlas image).
    // Normalized on purpose: UVs are not lengths, so DrawList::scale
    // (the uiScale transform) deliberately leaves them untouched.
    Rect uvRect{};
    // CustomDraw payload. For CUSTOM_TRACE_BORDER this is the complete
    // shader-border interface: rect + cornerRadius + borderWidth + phase
    // (caller-supplied animation time) + the three trace colors. `color`
    // carries traceRed with its alpha already scaled by traceOpacity.
    std::uint32_t tag = 0;
    float phase = 0.0f;
    float borderWidth = 0.0f;
    Color traceA{}; // red
    Color traceB{}; // blue
    Color traceC{}; // purple
};

// Well-known CustomDraw tags. Tag 1 is the animated Trace border pass: on
// Vulkan and GL33 the back end renders the shader-driven circuit trace
// (SDF rounded-rect border, dash pattern animated by `phase`); the
// software back end draws phase-baked frames from the same pattern, or the
// flat Polyline emitted just before this command as its starting fallback.
constexpr std::uint32_t CUSTOM_TRACE_BORDER = 1;

class DrawList {
public:
    void pushClip(Rect r);
    void popClip();
    void fillRect(Rect r, Color c, float cornerRadius = 0.0f);
    void line(Vec2 a, Vec2 b, Color c, float thickness = 1.0f);
    void polyline(const std::vector<Vec2>& pts, Color c, float thickness = 1.0f);
    // The same command without an intermediate std::vector: the
    // points are copied straight from the caller's array (round-3
    // review PERF-3 - hot draw code was building a temporary
    // vector per polyline, then copying it again).
    void polylineRange(const Vec2* pts, std::size_t n, Color c, float thickness = 1.0f);
    void textRun(Rect origin, const std::string& s, const TextStyle& style, Color color,
                 float baseline);
    void imageQuad(Rect r, int textureId, float alpha = 1.0f);
    // One rasterized glyph: destination rect (same unit space as
    // every other command), atlas sub-rect in normalized UV, atlas id
    // in textureId, tint in color. Emitted by the glyph expansion
    // pass (expandGlyphRuns), not by widgets.
    void glyphQuad(Rect dest, Rect uv, int atlasId, FontId font, Color color);
    void custom(std::uint32_t tag, Rect r, float phase);
    // Emits the fallback Polyline AND the CustomDraw trace-border hook with
    // the full shader payload, in that order.
    void traceBorder(Rect r, const Style& style);

    const std::vector<DrawCmd>& commands() const { return cmds_; }
    void clear() { cmds_.clear(); }
    // Swaps the whole command vector; the glyph expansion pass uses
    // it to replace its input list with the expanded one in place.
    void swapCommands(std::vector<DrawCmd>& other) { cmds_.swap(other); }
    // Multiplies every spatial field of every command by s: rects,
    // points, corner radii, line/polyline thickness, text size /
    // letter spacing / baseline, and the CustomDraw border width.
    // Colors, ids, weights, text, and the CustomDraw `phase` (a
    // dimensionless animation parameter, NOT a length) are untouched.
    // This is the uiScale output transform; UiContext::emitDraw applies
    // it to the finished list. The implementation switches over every
    // DrawCmd::Kind with no default case, so adding a command kind
    // without updating it trips -Wswitch under -Wall.
    void scale(float s);
    // The affine form of scale(): every spatial field is multiplied
    // by s and then translated by `offset` (positions only: rect
    // origins and polyline/line points; sizes, radii, thicknesses
    // and text metrics are scaled, never translated). scale(s) is
    // exactly transform(s, {0, 0}). Frame uses this to lift a
    // subtree drawn in design space into its content box.
    void transform(float s, Vec2 offset);
    // Moves every command of `other` onto the end of this list
    // (other is left empty). Frame::draw uses it to splice its
    // transformed subtree into the frame's own list.
    void absorb(DrawList& other);

private:
    std::vector<DrawCmd> cmds_;
};

class Widget;
class UiContext;
class Popup;
class ListBox;

// Widget liveness registry (round-2 review C1/C2): every Widget
// construction clears and every destruction sets a tombstone for
// its address, so UiContext can answer "is this pointer still a
// live widget" by pointer comparison alone, WITHOUT dereferencing
// the widget... which is exactly what must never happen to a
// widget a handler destroyed mid-dispatch (the tree root itself
// may be the dead widget, so even walking from the root is out).
// Address reuse is safe: a widget constructed at a recycled
// address clears the tombstone in its constructor.
void noteWidgetConstructed(const Widget* w);
void noteWidgetDestroyed(const Widget* w);
bool widgetTombstoned(const Widget* w);

// ----------------------------------------------------------- glyph source
// The glyph draw contract (2026-11-01). TextRun is the layout-level
// contract; GlyphQuad is the pixel-level one. When a glyph source is
// bound to a UiContext, emitDraw runs expandGlyphRuns over the
// finished list: every TextRun the source can fully cover (after
// fallback segmentation with the context's metrics provider) is
// replaced, in place and in order, by one GlyphQuad per code point.
// A run with any uncovered code point stays a TextRun, so a back end
// always sees either real glyphs or the honest abstract run, never a
// half-expanded mix. Back ends consume GlyphQuad like ImageQuad with
// a coverage texture: sample the atlas at uvRect, multiply by color.
class IGlyphSource {
public:
    virtual ~IGlyphSource() = default;
    // Fills the quad for one code point whose pen origin sits at
    // (penX, baselineY) in draw-list units. Returns false when this
    // source has no rasterized glyph for the code point in that font.
    // Const because expansion runs inside the const emitDraw; a
    // source that rasterizes on demand keeps its atlas cache mutable.
    virtual bool glyphQuad(char32_t cp, FontId font, float size, int weight,
                           float penX, float baselineY,
                           Rect& outDest, Rect& outUv, int& outAtlas) const = 0;
    // NON-baking availability queries for the two-pass expansion
    // (review SUP-4): glyphAvailable reports whether this source has
    // a glyph for the code point at all; glyphNeedsNewCell reports
    // whether producing it would consume a fresh atlas cell (false
    // when already baked, and false when unavailable); freeGlyphCells
    // is the remaining cell capacity. None of them bake anything.
    virtual bool glyphAvailable(char32_t cp, FontId font, float size, int weight) const = 0;
    virtual bool glyphNeedsNewCell(char32_t cp, FontId font, float size, int weight) const = 0;
    virtual int freeGlyphCells() const = 0;
};

// The expansion pass itself: rewrites `dl` in place per the contract
// above, using `metrics` for fallback segmentation and advances (the
// shell binds the same provider as metrics and glyph source, so the
// two can never disagree).
void expandGlyphRuns(DrawList& dl, const IGlyphSource& src, const ITextMetrics& metrics);

// Per-frame draw context handed down the tree. `opacity` accumulates the
// global uiOpacity and per-widget overrides; it applies to background
// fills AND text (round-2 review S3: text runs receive the effective
// opacity at emission, so text fades with its window exactly like
// fills and images do).
struct DrawParams {
    const Style* style = nullptr;
    const ITextMetrics* metrics = nullptr;
    const Widget* focused = nullptr;
    float opacity = 1.0f;
    Color fill(Color c) const {
        c.a *= opacity;
        return c;
    }
};

// ------------------------------------------------------------ state words
// Widget state is packed into ONE QUAD WORD per widget (JD doctrine,
// 2026-11-01: the whole engine uses bit manipulation because it is
// some of the fastest math on a CPU). The word is a
// std::atomic<std::uint64_t> holding every boolean state as a named
// bit and every small enum state as a shifted field, so a state reads
// with a mask test, flips with a single XOR read-modify-write, and a
// state CHANGE between two snapshots is exactly the XOR of the words.
// Bit transitions are single atomic fetch_or / fetch_and / fetch_xor
// RMWs; shifted-field writes are documented at Widget::storeField.
// Never a lock, never a CAS loop: there are no mutexes anywhere in
// this library. The word is the cross-thread surface of the GUI (a
// worker may raise a bit; the UI thread consumes snapshots). Layout
// and draw order never depend on the word's timing: order is
// insertion order, always, and the simulation-facing contract stays
// snapshot/intent based.
//
// Bit allocation map (stable: bits 0..11 kept their numbers when the
// word widened from DWORD to QWORD in the state-packing pass):
//   bits  0..15  core interaction state (WS_* below; 0..11 assigned,
//                12..15 reserved for future core bits)
//   bits 16..31  common Widget flags (WS_VISIBLE and friends below;
//                16..21 assigned) AND the common widget SETTINGS
//                fields (build 19, JD's settings-bitmask doctrine:
//                per-widget settings live in this word so hot paths
//                read them with a mask test, never a function call):
//                bits 22..27 the textPad field (whole design units,
//                0..63, default 5), bits 28..29 the text alignment
//                field (0 = Left, 1 = Center, 2 = Right, default
//                Center), bits 30..31 reserved for future settings.
//   bits 32..63  widget-specific state, owned and documented at each
//                class. BUILD 27 RE-MAP (JD's universal Frame
//                inheritance): Frame now sits in EVERY widget's
//                ancestry, so Frame's own allocation comes first in
//                every word and every derived class's bits moved
//                above it - before this build most classes owned
//                bit 32 (and fields up to 37) directly, which
//                collided with Frame's scroll state the moment the
//                hierarchies merged. The map now reads:
//                  bits 32..57  Frame (every widget): scrollable
//                    (32) and the scroll offset field (33..57, 25
//                    bits, unsigned fixed point, 1/256 design
//                    units - the field held 33..63 before the
//                    merger; 25 bits still reach 131,072 units).
//                  bits 58..63  the derived class's own, allocated
//                    per HIERARCHY as before: Window has-frame (58);
//                    BootWindow (above Window) stage field (59..60),
//                    has-target (61), complete-fired (62); Popup
//                    has-frame (58); TextField multi-line (58);
//                    ScrollArea windowed (58); ProgressBar
//                    show-percent (58); SysterianDecoder
//                    complete-fired (58); ItemSlot selected (58);
//                    FileList files-muted (58) and
//                    press-generation-valid (59); Collapsible
//                    body-pad field (58..63).
//                Bit numbers still repeat across UNRELATED classes
//                on purpose: every widget owns its own word. The
//                range is allocated per class HIERARCHY, though: a
//                derived widget's bits start above every base
//                class's, because the whole hierarchy shares the one
//                word (BootWindow's field sits above Window's
//                has-frame bit for exactly this reason; the
//                collision was caught by the boot tests during this
//                pass and is why the rule is written down here).
constexpr std::uint64_t WS_HOVERED  = 1ull << 0;  // pointer is over the widget
constexpr std::uint64_t WS_PRESSED  = 1ull << 1;  // pointer went down, not yet up
constexpr std::uint64_t WS_FOCUSED  = 1ull << 2;  // widget holds keyboard focus
constexpr std::uint64_t WS_DISABLED = 1ull << 3;  // widget is disabled
constexpr std::uint64_t WS_EXPANDED = 1ull << 4;  // Collapsible / tree node open
constexpr std::uint64_t WS_SELECTED = 1ull << 5;  // RadioButton / row selected
constexpr std::uint64_t WS_ACTIVE   = 1ull << 6;  // active page / engaged control
constexpr std::uint64_t WS_ON       = 1ull << 7;  // Toggle value is on
constexpr std::uint64_t WS_CHECKED  = 1ull << 8;  // CheckBox is checked
constexpr std::uint64_t WS_OPEN     = 1ull << 9;  // Popup / ComboBox menu is open
constexpr std::uint64_t WS_DRAGGING = 1ull << 10; // drag in progress (thumb/title)
constexpr std::uint64_t WS_COMPOSING = 1ull << 11; // IME composition live (TextField)
// Common Widget flags (bits 16..31).
constexpr std::uint64_t WS_VISIBLE     = 1ull << 16; // Widget::visible()
constexpr std::uint64_t WS_BG_OVERRIDE = 1ull << 17; // a background override is set
constexpr std::uint64_t WS_STYLE_NOTED = 1ull << 18; // a last-used Style was noted
constexpr std::uint64_t WS_ABS_PLACED  = 1ull << 19; // an absolute parent placed this widget
constexpr std::uint64_t WS_ABS_PRIMED  = 1ull << 20; // this absolute container has primed
constexpr std::uint64_t WS_TRACE_BORDER = 1ull << 21; // emit the Trace border in draw

// ---------------------------------------------------------------- anchors
// Per-widget anchor word, applied by ABSOLUTE containers
// (LayoutDir::None). When the container's inner size changes by
// (dx, dy), a placed child's rect follows its anchored edges:
//   left only:    x unchanged        right only:   x += dx
//   left + right: x unchanged, w += dx
//   neither:      x += dx / 2 (the control floats at the proportional
//                 middle, the classic unanchored behavior)
// and the same for top/bottom with dy. A child anchored to all four
// edges stretches with the container: that is the absolute-mode
// equivalent of box layout's flex fill. The container's origin motion
// always carries every child rigidly; anchors govern SIZE changes
// only. In Row/Column box containers the anchor word is ignored:
// pinning and stretching there are expressed by fixed sizes, margins
// and flex (documented in README, "Anchoring").
constexpr std::uint32_t ANCHOR_LEFT   = 1u << 0;
constexpr std::uint32_t ANCHOR_TOP    = 1u << 1;
constexpr std::uint32_t ANCHOR_RIGHT  = 1u << 2;
constexpr std::uint32_t ANCHOR_BOTTOM = 1u << 3;

// ------------------------------------------------------------------ widget

enum class LayoutDir { None, Row, Column };

class Widget {
public:
    explicit Widget(std::string id) : id_(std::move(id)) {
        noteWidgetConstructed(this);
    }
    virtual ~Widget() { noteWidgetDestroyed(this); }

    const std::string& id() const { return id_; }
    int intId() const { return intId_; }
    void setIntId(int v) { intId_ = v; }

    Widget* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    // Takes ownership; children are stored (and everywhere walked) in
    // insertion order. Returns the added widget.
    Widget* addChild(std::unique_ptr<Widget> child);
    Widget* findById(const std::string& id); // depth-first, insertion order

    bool visible() const { return hasState(WS_VISIBLE); }
    void setVisible(bool v) { writeState(WS_VISIBLE, v); }
    bool enabled() const { return !hasState(WS_DISABLED); }
    void setEnabled(bool v) { writeState(WS_DISABLED, !v); }
    virtual bool focusable() const { return false; }

    // The widget's state word (see WS_* above): snapshot it, mask it,
    // XOR two snapshots to see exactly what changed.
    std::uint64_t stateWord() const { return stateWord_.load(std::memory_order_relaxed); }
    bool hasState(std::uint64_t bits) const { return (stateWord() & bits) != 0u; }

    // ---- Settings fields (build 19, JD ruling 2026-11-02) --------
    // JD, from his Build 18 hands-on test: "title bars text bars
    // and everything should have a five pixel pad in between their
    // bounding boxes on every side... any text field any title bar
    // or anything like that should always have a five pixel pad top
    // bottom left right and that should be exposed as a setting in
    // the widgets", "even radio buttons and check boxes should
    // have that five pixel pad left right top and bottom so that
    // the spacing is always good", and "remember to keep our
    // settings bitmask so that we can cut down on function calls
    // later they're atomic bitmask". The addendum widened the pad
    // to EVERY control ("just give every control some kind of a
    // pad on it") and added the alignment default: text controls
    // are CENTERED by default, left/right selectable per instance,
    // with TreeView rows and Collapsible (expandable) headers -
    // and, by the same list-family reading, ListBox and
    // VirtualList rows - defaulting to Left in their constructors.
    //
    // textPad is the distance between the widget's CONTENT (text
    // runs, control glyphs, discrete graphical content) and its
    // bounding box, on EACH side, in design units: content is laid
    // out inside the padded box (see textRect()), so the pad scales
    // with the widget and the spacing sits right at any size.
    // Box-FILLING layers are not content: a widget's background
    // fill, a progress bar's fill and an image's quad span the
    // whole rect exactly as before - the pad seats what sits IN
    // the box, never the box's own paint. The Edges `padding`
    // member below is a different mechanism (a container's inset
    // for laying out its CHILDREN) and Frame::contentPad is a
    // third (a frame's design area vs its outline); the three are
    // documented together in README, "Padding: three mechanisms".
    //
    // Both settings live in the state word's common settings
    // range (see the allocation map at the WS_* block): textPad as
    // a 6-bit field of WHOLE design units (0..63) at bits 22..27,
    // alignment as a 2-bit field at bits 28..29. Reads are one
    // loadField mask+shift inline; writes are storeField's two
    // RMWs, so a settings write can never clobber a concurrent
    // state bit on the same word. The defaults are baked into the
    // word's initial value: pad 5, alignment Center.
    static constexpr int kTextPadShift = 22;
    static constexpr std::uint64_t kTextPadMask = 0x3Full << kTextPadShift;
    static constexpr int kTextAlignShift = 28;
    static constexpr std::uint64_t kTextAlignMask = 0x3ull << kTextAlignShift;
    static constexpr float kDefaultTextPad = 5.0f;
    float textPad() const {
        return static_cast<float>(loadField(kTextPadMask, kTextPadShift));
    }
    // Sets the pad in design units, quantized to whole units and
    // clamped to the field's range (0..63). A non-finite value is
    // refused and the last sane pad stands (the Frame::setContentPad
    // validation discipline).
    void setTextPad(float v) {
        if (!std::isfinite(v)) return;
        float q = std::floor(v + 0.5f);
        q = q < 0.0f ? 0.0f : (q > 63.0f ? 63.0f : q);
        storeField(kTextPadMask, kTextPadShift, static_cast<std::uint64_t>(q));
    }
    TextAlign textAlign() const {
        switch (loadField(kTextAlignMask, kTextAlignShift)) {
            case 0: return TextAlign::Left;
            case 2: return TextAlign::Right;
            default: return TextAlign::Center; // 1, and the reserved 3
        }
    }
    void setTextAlign(TextAlign a) {
        storeField(kTextAlignMask, kTextAlignShift,
                   a == TextAlign::Left ? 0ull : (a == TextAlign::Right ? 2ull : 1ull));
    }

    // Layout inputs.
    LayoutDir dir = LayoutDir::None; // container direction for children
    float fixedW = -1.0f;            // <0 = auto
    float fixedH = -1.0f;
    float flex = 0.0f;               // >0 = share of leftover main-axis space
    Edges margin{};
    Edges padding{};
    // Anchor word (ANCHOR_* bits) for absolute (LayoutDir::None)
    // parents; ignored by box layout. Default: pinned top-left.
    std::uint32_t anchors = ANCHOR_LEFT | ANCHOR_TOP;
    // Layout exemption for composed chrome (build 14): a parent
    // container never places an exempt child - no box row, no
    // absolute prime, no anchor deltas. The composing widget
    // (Window, for its TitleBar) lays the child out itself on
    // every layout. The child still draws and hit-tests as a
    // normal child; only its PLACEMENT is the owner's business.
    // (Without this, a TitleBar inside a Column-direction Window
    // - the inventory screen is one - would eat a layout row and
    // shift the whole screen.)
    bool layoutExempt = false;
    // Per-widget opacity override for background fills: <0 inherits,
    // otherwise multiplies the inherited opacity. Windows use this as
    // their per-window opacity setting.
    float opacityOverride = -1.0f;
    // Optional text style override for widgets that draw text.
    std::optional<TextStyle> textStyle;

    const Rect& rect() const { return rect_; }
    // This widget's rect in TREE (screen) coordinates: rect_
    // with every ancestor Frame's transform applied, innermost
    // frame first, so nested frames compose exactly as drawing
    // does. Inside an unframed subtree it equals rect_. Popups
    // live in tree coordinates: a widget that opens one MUST
    // anchor it from treeRect(), never from rect_ - inside a
    // Frame, rect_ is design coordinates (the FRM-2 contract;
    // the ComboBox menu floated off in design space until
    // build 18 routed it through here).
    Rect treeRect() const;
    // The Style this widget was last laid out or drawn with. Event
    // handlers that must re-layout or map pointer coordinates use it
    // instead of defaultStyle(), so what the user saw is what the
    // handler computes with (review CORE-M5 / CORE-L2 / CORE-L6).
    // Falls back to defaultStyle() before the first layout/draw.
    const Style& lastStyle() const {
        return hasState(WS_STYLE_NOTED) ? lastStyle_ : defaultStyle();
    }
    // A width the parent is about to allot, passed before desired
    // size queries so width-dependent desired heights (wrapped text)
    // are correct on the FIRST layout pass (review CORE-M1). Set by
    // box layout; -1 = no hint.
    void setDesiredWidthHint(float w) { desiredWidthHint_ = w; }
    float desiredWidthHint() const { return desiredWidthHint_; }
    // Assigns this widget's rect and lays out its subtree.
    virtual void layout(Rect r, const Style& style = defaultStyle());
    // Assigns ONLY this widget's rect, without laying out children.
    // For containers that manage their children's rects themselves
    // (the windowed ScrollArea lays out its content's visible rows
    // directly); ordinary code uses layout().
    void setRect(Rect r) { rect_ = r; }
    // Intrinsic sizes used by parents when fixed size is auto and flex is 0.
    virtual float desiredHeight(const Style& style) const;
    virtual float desiredWidth(const Style& style) const;

    // Event handlers. Return true when handled; unhandled events bubble to
    // the parent. Handlers express results by invoking the widget's
    // callbacks (intents), never by mutating application state directly.
    virtual bool handlePointer(const PointerEvent&) { return false; }
    virtual bool handleKey(const KeyEvent&) { return false; }
    // An OUTSIDE press: a pointer Down that hit this widget's
    // parent (a Frame's letterbox) rather than this widget
    // itself, delivered in this widget's own coordinate space.
    // This is a dedicated channel, NOT handlePointer: a widget
    // only ever hears about outside presses by overriding this,
    // so a press that missed can never arm a Button or scroll an
    // unpointed ScrollArea (round-3 review FRM-4). The default
    // ignores it. Root/screen widgets override it to implement
    // press-outside-to-cancel semantics.
    virtual bool handleOutsidePress(const PointerEvent&) { return false; }
    // True when draw() clips this widget's CHILDREN to its own rect
    // (PushClip in draw: ScrollArea, TreeView, ListBox, VirtualList,
    // and a multi-line TextField). Hit testing consults it (round-2
    // review C9): children of a clipping parent are NOT hittable
    // outside the parent's rect, while children of a non-clipping
    // parent keep the CORE-M7 overflow rule (drawn there, hittable
    // there).
    virtual bool clipsChildren() const { return false; }
    // Called by a UiContext that is being destroyed while this
    // widget holds a back-pointer to it (ComboBox::ctx_), so the
    // widget can drop the pointer before it dangles (round-2 review
    // C3). Default: no back-pointer, nothing to do.
    virtual void detachContext(UiContext*) {}

    // Emits this widget's draw commands, then its children's, in insertion
    // order. Implementations first fold opacityOverride into the params
    // they pass on.
    virtual void draw(DrawList& dl, const DrawParams& params) const;

    // Optional background override; when unset, the class default from the
    // Style is used. Setting an alpha-0 color suppresses the fill entirely.
    void setBackground(Color c) { bgOverride_ = c; raiseState(WS_BG_OVERRIDE); }
    // Emit the Trace border polyline + CustomDraw hook in draw.
    // A packed common flag (WS_TRACE_BORDER, bit 21).
    bool traceBorder() const { return hasState(WS_TRACE_BORDER); }
    void setTraceBorder(bool v) { writeState(WS_TRACE_BORDER, v); }

protected:
    void noteStyle(const Style& s) const { lastStyle_ = s; raiseState(WS_STYLE_NOTED); }
    // Lock-free state-word transitions (atomic read-modify-write).
    void raiseState(std::uint64_t bits) const {
        stateWord_.fetch_or(bits, std::memory_order_relaxed);
    }
    void dropState(std::uint64_t bits) const {
        stateWord_.fetch_and(~bits, std::memory_order_relaxed);
    }
    void flipState(std::uint64_t bits) const {
        stateWord_.fetch_xor(bits, std::memory_order_relaxed);
    }
    void writeState(std::uint64_t bits, bool on) const {
        if (on) raiseState(bits); else dropState(bits);
    }
    // Packed-field access for the small enum states living in the
    // word's widget-specific range: loadField masks + shifts one
    // snapshot; storeField writes the field as TWO RMWs (fetch_and
    // with the clear mask, then fetch_or with the shifted value),
    // never a composed load/store (round-2 review S1: a composed
    // store could erase a concurrent bit RMW on the same quad word,
    // breaking the cross-thread contract above). Two RMWs still
    // never clobber another bit; the field itself stays
    // single-logical-owner (two racing FIELD writers could
    // interleave their and/or pairs), which is why fields live on
    // the UI thread's mutation path (e.g. BootWindow's stage).
    std::uint64_t loadField(std::uint64_t mask, int shift) const {
        return (stateWord() & mask) >> shift;
    }
    void storeField(std::uint64_t mask, int shift, std::uint64_t value) const {
        stateWord_.fetch_and(~mask, std::memory_order_relaxed);
        stateWord_.fetch_or((value << shift) & mask, std::memory_order_relaxed);
    }

    Color background(const Style& style) const {
        return hasState(WS_BG_OVERRIDE) ? bgOverride_ : defaultBackground(style);
    }
    virtual Color defaultBackground(const Style&) const { return Color{}; }
    DrawParams localParams(const DrawParams& params) const;
    void drawBackgroundAndBorder(DrawList& dl, const DrawParams& params) const;
    // The widget's effective text style: its override if set, else the
    // Style's body text; palette text color fills an alpha-0 style color.
    // The style's align is REPLACED by the widget's alignment setting
    // (build 19): alignment is a per-widget setting in the state word,
    // so a Style- or override-supplied align no longer steers a
    // widget - callers set the instance's setTextAlign instead.
    TextStyle effectiveTextStyle(const Style& style) const;
    Rect innerRect() const;
    // The widget's content box (build 19): rect_ inset by textPad on
    // every side. Text runs and control glyphs are laid out inside
    // this box, so no content ink ever sits closer than textPad to
    // the rect's edge on any side, at any scale. Sizes clamp at 0.
    Rect textRect() const {
        const float p = textPad();
        return {rect_.x + p, rect_.y + p,
                std::max(0.0f, rect_.w - 2.0f * p),
                std::max(0.0f, rect_.h - 2.0f * p)};
    }
    // The horizontal anchor offset for content of width `contentW`
    // inside a box of width `boxW`, per the alignment setting:
    // Left 0, Center half the slack, Right all of it; slack clamps
    // at 0, so over-wide content left-anchors and overflows right,
    // the way Label's aligned lines have always behaved.
    float alignOffset(float contentW, float boxW) const {
        switch (textAlign()) {
            case TextAlign::Left: return 0.0f;
            case TextAlign::Right: return std::max(0.0f, boxW - contentW);
            case TextAlign::Center: return std::max(0.0f, (boxW - contentW) * 0.5f);
        }
        return 0.0f;
    }

    friend class UiContext; // the context raises focus/hover bits

    std::string id_;
    int intId_ = 0;
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    // The quad state word (see the WS_* allocation map). It starts
    // with WS_VISIBLE raised: a widget is visible until hidden.
    // Visibility, the background-override flag, the style-noted
    // flag and the absolute-placement flags (WS_ABS_PLACED for this
    // widget, WS_ABS_PRIMED for this widget AS an absolute
    // container) all live here, not in plain bool fields. The
    // build-19 settings defaults ride the same initial value:
    // textPad 5 in its field (bits 22..27) and alignment Center
    // (1) in its field (bits 28..29), so a fresh widget carries
    // JD's defaults before any setter runs.
    mutable std::atomic<std::uint64_t> stateWord_{
        WS_VISIBLE | (std::uint64_t{5} << kTextPadShift) |
        (std::uint64_t{1} << kTextAlignShift)};
    mutable float lastLayoutW_ = -1.0f; // width at last layout (Label wrap)
    // Absolute-placement data: the inner rect at the previous
    // layout, the baseline anchor deltas are measured from. (The
    // placed/primed flags are WS_ABS_PLACED / WS_ABS_PRIMED.)
    Rect lastInner_{};
    mutable Style lastStyle_{};
    float desiredWidthHint_ = -1.0f;
    Rect rect_{};
    Color bgOverride_{};
};

// Easing functions (defined with the animation helpers in the
// .cpp; declared here because Frame's animation API defaults to
// them). t in [0, 1] -> eased t in [0, 1].
namespace anim {
float easeInOutCubic(float t);
float easeOutCubic(float t);
} // namespace anim

// ---------------------------------------------------------------- Frame
// A design-space container (JD directive, 2026-11-02, from his
// on-device test of the inventory in demo build 8: on a short
// landscape phone canvas the inventory's fixed-height Agent
// column overflowed the bottom of the screen, because every
// screen had to be re-solved - and re-fitted by hand - for each
// new viewport shape). A Frame ends that: widgets are set in a
// Frame ONCE, in the Frame's DESIGN space, and the Frame scales
// everything inside it to fit its own rect on any screen,
// automatically, because that is what a frame does.
//
// Contract:
//   - The Frame has a design size (designWidth x designHeight,
//     settable; default 1280x800, the inventory's approved
//     desktop layout). Children are laid out in DESIGN
//     coordinates: placement follows the same rule an absolute
//     (LayoutDir::None) container primes with - a child's
//     position comes from its margins, its size from its fixed
//     size or the remaining design space - and the anchor word
//     applies when the DESIGN size changes between layouts, the
//     same deltas an absolute container would apply. A child
//     with no fixed size fills the design space (that is the
//     inventory's case: one window, filling its frame).
//   - layout(rect): the uniform fit scale is
//     min(rect.w / designW, rect.h / designH); the scaled
//     design area (the CONTENT BOX) is CENTERED inside rect,
//     letterboxed on the long axis. Children keep their design
//     rects: layout, draw-internal geometry and cached metrics
//     inside the subtree never see the frame's scale.
//   - Content pad (build 13): setContentPad keeps a margin, in
//     DESIGN units, clear between the scaled design area and
//     the Frame's own outline on all four sides - the fit
//     treats the design area as design + 2*pad on each axis,
//     so the content box lands pad*scale inside the frame's
//     rect and the subtree's design geometry is untouched.
//     The pad ring is letterbox: presses there take the
//     outside-press channel like any other non-content area.
//     Default 0: the design area fits the frame's rect
//     exactly, the behavior every frame had before the pad
//     existed.
//   - draw(): the Frame draws its own background and (when set)
//     its Trace outline at its own rect; the subtree is drawn
//     into a scratch list in design space and spliced in through
//     DrawList::transform(scale, content origin), clipped to
//     the content box - geometry, text sizes and stroke widths
//     all scale, and nothing inside the frame can paint outside
//     its outline.
//   - Input: UiContext hit testing maps a point through the
//     Frame before consulting children (a press in the
//     letterbox never reaches them), and pointer dispatch maps
//     the event into design coordinates for every handler
//     inside the Frame (and back out for handlers at and above
//     it), so hit testing, clicks, drags and touch long-press
//     all work at any frame scale with no per-widget code.
//     A press in the letterbox hits the Frame itself; the
//     Frame delivers it, mapped, through the dedicated
//     handleOutsidePress channel (never handlePointer), which
//     is how a held inventory stack still cancels on a tap
//     outside the framed area. Key events carry no coordinates
//     and pass through untouched.
//   - POPUPS ARE TREE-SPACE (round-3 review FRM-2, recorded
//     contract): popups live on the UiContext stack, outside
//     any Frame - they are NOT transformed or clipped by a
//     Frame, and a popup opened from inside a Frame appears at
//     tree coordinates, not design coordinates. A framed
//     widget that opens a popup anchors it from its
//     treeRect() (Widget::treeRect walks the ancestor Frames,
//     innermost first - build 18 made the mechanism general;
//     the ComboBox menu is the first rider, converted from the
//     hand-rolled single-frame toTree the demo's Play popup
//     used). Transforming popups through their owning Frame
//     remains out of scope; this is the stated contract.
//   - SCROLLING (build 20, JD ruling from his Build 18 test:
//     the gallery's collapsibles sub-frame clipped a line of
//     text at its bottom edge - "frames should be scrollable
//     so if something is expanding outside of a frame's
//     bounding box just give the frame a scrollbar where if
//     we're inside there we can scroll down and see it";
//     "just make frames have a scrollable property so things
//     can expand below where the frame goes but our mouse can
//     let us scroll it"). A Frame with the scrollable property
//     set lets its content grow BELOW the visible box: the
//     children anchor and lay out against a CONTENT box whose
//     height (contentHeight()) is the design height extended
//     to the laid-out extent of the subtree, and the visible
//     box is a window into that content, offset by scrollY.
//     The offset is part of the Frame's coordinate mapping, so
//     drawing (the same transform, shifted), hit testing, the
//     dispatch mapping and treeRect() all agree about where a
//     scrolled child sits: a control scrolled into view is
//     clickable where it is SEEN, and a control scrolled out
//     is not clickable through the clip. When the content
//     overflows, an overlay scrollbar stands at the content
//     box's right edge (no layout ever shifts for it); the
//     wheel over the content scrolls (a self-scrolling child
//     under the pointer consumes the wheel first, by ordinary
//     bubble order), the thumb drags with the grab point held
//     under the cursor, and a press on the track pages by one
//     visible height. Default OFF: an unscrollable Frame is
//     byte-for-byte the Frame it was before the property
//     existed. Full contract: docs/ANCHORING.md, "Scrollable
//     frames".
//   - The debug outlines sit over the Frame's OWN subtree;
//     popups and later-drawn siblings paint over them
//     (round-3 review FRM-9, wording narrowed to match).
//   - Debug (JD, same day): with Style::debugShowFrameOutlines
//     set, draw() additionally outlines the Frame's own box in
//     magenta and the content box in yellow, over the content,
//     so bounding boxes can be checked on a device. With the
//     flag off the emitted list is byte-for-byte the list the
//     Frame emitted before the flag existed.
class Frame : public Widget {
public:
    explicit Frame(std::string id, float designW = 1280.0f, float designH = 800.0f)
        : Widget(std::move(id)), designW_(saneDesign(designW, 1280.0f)),
          designH_(saneDesign(designH, 800.0f)) {
        dir = LayoutDir::None;
        setBackground(Color{0, 0, 0, 0}); // clear: the content paints the area
    }
    // ---- Universal inheritance (build 27, JD ruling 2026-11-04:
    // "Make every widget inherit Frame") -------------------------
    // Every concrete widget class in this library now derives
    // from Frame, so the Frame facilities (content pad, elastic
    // fit, scroll state, animation) sit in every widget's ancestry
    // and every widget "lives inside a frame", per the ruling.
    // The viewport machinery itself - the design-space fit, the
    // letterbox, the coordinate transforms the hit tester and
    // treeRect() apply - runs only on an ENGAGED frame: one
    // constructed as a Frame (above) or given an explicit design
    // size (setDesignSize below). A widget class's constructor
    // chains to the inert constructor instead, and an inert frame
    // delegates layout, draw, pointer handling and desired sizes
    // to Widget's implementations untouched, so a leaf widget that
    // carries no framed children renders, hit-tests and lays out
    // exactly as it did before the merger. Engagement is also the
    // discriminator every dynamic_cast<Frame*> site in the
    // library now checks (frameEngaged()), where "is a Frame"
    // used to be the test: unengaged widgets are transparent to
    // coordinate mapping, exactly as non-Frames were.
    struct InertFrameTag { explicit InertFrameTag() = default; };
    bool frameEngaged() const { return frameEngaged_; }

protected:
    // The base every widget class's constructor chains to: the
    // full Frame member set, disengaged - no design size (0x0),
    // no clear-background override, none of the viewport behavior.
    Frame(std::string id, InertFrameTag)
        : Widget(std::move(id)), designW_(0.0f), designH_(0.0f),
          frameEngaged_(false) {}

public:
    // Design sizes are validated like Style values (round-3
    // review FRM-6): a NaN or infinite design size would poison
    // the content box permanently (Inf * 0 = NaN), and a
    // non-positive one is meaningless. Invalid input is ignored
    // (the previous valid size stands); the constructor falls
    // back to the defaults for invalid arguments. Giving a widget
    // a design size ENGAGES its frame (build 27): that is the
    // moment its viewport machinery - fit, letterbox, transforms -
    // takes over its subtree.
    void setDesignSize(float w, float h) {
        if (std::isfinite(w) && w > 0.0f) designW_ = w;
        if (std::isfinite(h) && h > 0.0f) designH_ = h;
        if (designW_ > 0.0f && designH_ > 0.0f) frameEngaged_ = true;
    }
    float designWidth() const { return designW_; }
    float designHeight() const { return designH_; }
    // The last layout's fit: the uniform scale and the centered
    // content box, in the Frame's parent (tree) space.
    float contentScale() const { return scale_; }
    Rect contentRect() const { return content_; }
    // The content pad, in DESIGN units (see the class comment):
    // kept clear between the scaled design area and the Frame's
    // outline on all four sides. Validated like the design size
    // (round-3 review FRM-6 discipline): a NaN, infinite or
    // negative pad is refused and the last sane pad stands.
    void setContentPad(float pad) {
        if (std::isfinite(pad) && pad >= 0.0f) contentPad_ = pad;
    }
    float contentPad() const { return contentPad_; }
    // ELASTIC mode (build 18): an elastic Frame adopts its
    // effective layout box as its design size at every layout,
    // so its content scale is always exactly 1 and its design
    // space IS the space it is given. That makes a Frame usable
    // as a pure sub-framing boundary inside another Frame: the
    // ancestor owns the scaling, the elastic frame owns the
    // anchoring, the content pad and (with the debug flag) the
    // outline, and nested content composes without letterbox
    // bands or double scaling - JD's sub-frame construction for
    // the gallery, chat and files scenes. An elastic frame's
    // fixed design size is meaningless (it re-derives each
    // layout, before the fit, so the state is steady: the same
    // box in, the same design out), and box animation has no
    // visible effect on it - it is a boundary, not a viewport.
    // Used with a content pad, the pad still insets in the
    // frame's local units, which ARE tree units at scale 1.
    void setElastic(bool on) {
        elastic_ = on;
        if (on) frameEngaged_ = true; // an elastic boundary is a frame in use
    }
    bool elastic() const { return elastic_; }
    // ---- Scrolling (build 20; contract in the class comment
    // and docs/ANCHORING.md) -----------------------------------
    // The scrollable property is a settings bit in this Frame's
    // own state word (bit 32, widget-specific range), read and
    // written inline like every other packed setting - no new
    // virtuals, no function-call overhead in hot paths. Default
    // OFF, and off is exact: the offset reads as 0, the content
    // height is the design height, and every mapping, draw
    // command and hit test is the pre-scroll one.
    void setScrollable(bool on) {
        writeState(kScrollableBit, on);
        if (!on) { // a switched-off scroller stands at the top,
                   // its drag (if any) dead with the property
            storeField(kScrollYMask, kScrollYShift, 0);
            dropState(WS_DRAGGING);
        }
    }
    bool scrollable() const { return hasState(kScrollableBit); }
    // The scroll offset, in DESIGN units: how far the visible
    // window has travelled into the content. It lives in the
    // state word beside the property (bits 33..57) as unsigned
    // fixed point at 1/256 design units - the settings-bitmask
    // doctrine - so reads are one loadField and a snapshot of
    // the word carries the offset with everything else. Range
    // 0 .. 2^25/256 (131,072 units), far past any content height
    // this library will lay out; writes clamp to the scrollable
    // range and refuse non-finite values (the last sane offset
    // stands). The offset is STATE, never time: no animation,
    // no wall clock - a wheel notch, a drag or a set moves it
    // directly and it stays where it was put.
    float scrollY() const {
        return static_cast<float>(loadField(kScrollYMask, kScrollYShift)) *
               kScrollYQuantum;
    }
    void setScrollY(float y);
    // The content height, in design units: the design height,
    // extended at layout to the subtree's laid-out extent when
    // scrollable (see layout()). Before the first layout it
    // reads as the design height.
    float contentHeight() const { return contentH_ > 0.0f ? contentH_ : designH_; }
    // How far the window may travel: content past the fold.
    float maxScrollY() const {
        const float m = contentHeight() - designH_;
        return m > 0.0f ? m : 0.0f;
    }
    // True when the content overflows the visible box (a
    // scrollbar stands, wheels scroll, the strip is claimed).
    bool scrollOverflowing() const { return scrollable() && maxScrollY() > 0.0f; }
    // The scrollbar's width, in design units (the track hugs
    // the content box's right edge, overlaying the content).
    static constexpr float kScrollTrackW = 8.0f;
    // One wheel notch of travel, in design units - the same
    // step TreeView and ScrollArea have always used.
    static constexpr float kScrollWheelStep = 24.0f;
    // Hit test for the scrollbar strip, in CONTENT coordinates
    // (the space toDesign returns): the strip is the track's
    // column over the visible window's height. UiContext's hit
    // testing claims the strip for the Frame itself - the
    // chrome-over-children precedence the Window grip uses - so
    // a press there starts a scroll gesture, never a click on
    // the content painted underneath.
    bool scrollbarHit(float cx, float cy) const {
        if (!scrollOverflowing()) return false;
        if (cx < designW_ - kScrollTrackW || cx > designW_) return false;
        const float sy = scrollY();
        return cy >= sy && cy < sy + designH_;
    }
    // Pure coordinate mappings between tree space and design
    // space (no containment test: a tree point in the letterbox
    // maps to design coordinates outside the design rect, which
    // is exactly what drop-outside handling inside the frame
    // needs to see). For a scrollable Frame the design space IS
    // the content space: toDesign lands a visible point at its
    // content position (window position + scrollY), and toTree
    // is its exact inverse, so a scrolled child's tree rect is
    // where it is drawn. Unscrollable, the offset is 0 and the
    // mappings are the ones the Frame has always had.
    Vec2 toDesign(Vec2 treePoint) const {
        const float s = scale_ > 0.0f ? scale_ : 1.0f;
        return {(treePoint.x - content_.x) / s,
                (treePoint.y - content_.y) / s + scrollOffset()};
    }
    Vec2 toTree(Vec2 designPoint) const {
        return {content_.x + designPoint.x * scale_,
                content_.y + (designPoint.y - scrollOffset()) * scale_};
    }
    bool pointInContent(float x, float y) const { return content_.contains(x, y); }

    // Desired size honors an explicit fixed size first, like
    // every other widget (round-3 review FRM-7); unfixed, the
    // design size is the natural claim. An INERT frame (a widget
    // that has not engaged its frame, build 27) claims exactly
    // what the widget claimed before the merger: Widget's own
    // desired sizes.
    float desiredWidth(const Style& style) const override {
        if (!frameEngaged_) return Widget::desiredWidth(style);
        return fixedW >= 0.0f ? fixedW : designW_;
    }
    float desiredHeight(const Style& style) const override {
        if (!frameEngaged_) return Widget::desiredHeight(style);
        return fixedH >= 0.0f ? fixedH : designH_;
    }
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;

    // ---- Animation (build 14, JD directive: frames "need an
    // animate and scale and grow property that can do growing
    // and scaling over a time period"). Modeled on the BootWindow
    // expansion idiom: start values, target values, a duration,
    // an easing from the anim set, and a completion callback
    // that fires exactly once. Time comes ONLY from tick(dt) -
    // the shell feeds it, like every other clock in the library.
    // The animated geometry is a pure function of the elapsed
    // time accrued by tick: any tick slicing that reaches the
    // same elapsed time produces the same box and scale.
    //   - animateBoxTo: the frame's own box (its rect) grows or
    //     shrinks from its current box to the target box; the
    //     content fit follows the box, so the subtree scales
    //     with it. This is the grow property, and the framed
    //     boot scene's expansion runs on it.
    //   - animateScaleTo: the content scale multiplier (on top
    //     of the fit scale) interpolates to the target; the
    //     content box recenters at the new scale.
    //   - animateTo drives both channels in one animation.
    // Retargeting mid-animation: calling any animate* again
    // rebases the named channels from their CURRENT interpolated
    // values; channels the new call does not name freeze at
    // their current interpolated values. A replaced animation
    // never fires its completion. cancelAnimation freezes both
    // channels where they stand and fires nothing.
    // While an override stands (animating, or settled on a
    // target), layout() honors it over the rect it is handed -
    // until the parent hands a rect that is neither the last
    // handed rect nor the frame's current effective box (a
    // genuine parent move or resize), which always wins and
    // drops the override and any running animation with it.
    using Easing = float (*)(float);
    void animateBoxTo(Rect target, float durationSeconds, Easing easing = nullptr);
    void animateScaleTo(float targetScale, float durationSeconds, Easing easing = nullptr);
    void animateTo(Rect targetBox, float targetScale, float durationSeconds,
                   Easing easing = nullptr);
    void cancelAnimation();
    bool animating() const { return animActive_; }
    // The active animation's eased progress in [0, 1]; the
    // settled value when idle (0 before any animation, 1 after
    // one completes). The boot scene fades its content in with
    // this during the expansion.
    float animationProgress() const {
        return animActive_ ? easedAnimT() : animSettledProgress_;
    }
    // The content scale multiplier currently in force (1 = the
    // plain fit), animated or settled.
    float scaleMultiplier() const {
        return (animActive_ && animHasScale_) ? animScaleNow() : scaleMul_;
    }
    // Advances the animation (the ONLY time source) and re-runs
    // layout at the current geometry. A no-op when idle.
    void tick(float dtSeconds);
    // Fires exactly once when an animation reaches its target;
    // never for a replaced or cancelled one.
    std::function<void(Frame&)> onAnimationComplete;

private:
    static float saneDesign(float v, float fallback) {
        return (std::isfinite(v) && v > 0.0f) ? v : fallback;
    }
    // Animation helpers (build 14).
    float easedAnimT() const {
        const float raw = animDuration_ > 0.0f
                              ? std::min(1.0f, animElapsed_ / animDuration_)
                              : 1.0f;
        return animEasing_ ? animEasing_(raw) : anim::easeInOutCubic(raw);
    }
    Rect animBoxNow() const {
        const float t = easedAnimT();
        return {animFromBox_.x + (animToBox_.x - animFromBox_.x) * t,
                animFromBox_.y + (animToBox_.y - animFromBox_.y) * t,
                animFromBox_.w + (animToBox_.w - animFromBox_.w) * t,
                animFromBox_.h + (animToBox_.h - animFromBox_.h) * t};
    }
    float animScaleNow() const {
        const float t = easedAnimT();
        return animFromScale_ + (animToScale_ - animFromScale_) * t;
    }
    Rect effectiveBox() const {
        if (animActive_ && animHasBox_) return animBoxNow();
        return hasBoxOverride_ ? boxOverride_ : rect_;
    }
    void freezeAnimChannels();
    void beginAnim(Rect toBox, bool hasBox, float toScale, bool hasScale,
                   float durationSeconds, Easing easing);
    void relayoutSelf() {
        if (hasLayoutArg_) layout(lastLayoutArg_, lastStyle());
    }
    float designW_;
    float designH_;
    float contentPad_ = 0.0f;
    bool elastic_ = false;
    // Engagement (build 27; see the universal-inheritance note at
    // the constructors): true for a Frame constructed as a Frame
    // or given a design size / elastic mode, false for a widget
    // that merely carries the Frame base. Plain member, UI-thread
    // owned like the design size; layout/draw/hit-testing branch
    // on it exactly where they used to branch on "is a Frame".
    bool frameEngaged_ = true;
    float scale_ = 1.0f;
    Rect content_{};
    // Scrolling (build 20): the property bit and the offset
    // field in this Frame's own state word (widget-specific
    // range; see the allocation map at the WS_* block and the
    // accessors above).
    static constexpr std::uint64_t kScrollableBit = 1ull << 32;
    static constexpr int kScrollYShift = 33;
    // The offset field is 25 bits (33..57), not the 31 bits
    // (33..63) it held before build 27: with Frame in every
    // widget's ancestry, the top of the widget-specific range
    // (58..63) belongs to the derived classes, per hierarchy.
    // 25 bits at 1/256 still reaches 131,072 design units of
    // travel - orders past any content height this library lays
    // out. Full map: the WS_* allocation block.
    static constexpr std::uint64_t kScrollYMask = 0x1FFFFFFull << kScrollYShift;
    static constexpr float kScrollYQuantum = 1.0f / 256.0f;
    float scrollOffset() const { return scrollable() ? scrollY() : 0.0f; }
    void clampScroll(); // re-clamp the stored offset into range
    // One placement pass over the children against `inner` (the
    // design/content inner rect): the prime + anchor-delta
    // machinery layout() has always run, factored out because a
    // scrollable Frame places twice when its content height
    // settles (see layout()).
    void placeChildren(const Rect& inner, const Style& style);
    // The laid-out extent of the subtree, in content
    // coordinates: the lowest bottom edge (margin included)
    // among visible descendants, not descending into clipping
    // widgets or nested Frames, whose interiors are bounded by
    // their own boxes.
    float measureContentExtent() const;
    // The scrollbar thumb's placement within the visible
    // window, in design units: its top (relative to the
    // window's top) and its height.
    void scrollThumbMetrics(float& top, float& h) const;
    float contentH_ = 0.0f; // settled content height; 0 = unset
    float dragStartY_ = 0.0f;      // thumb drag: the press point,
    float dragStartScroll_ = 0.0f; // in parent space, and the
                                   // offset when it landed
    // Animation state (build 14): plain members, UI-thread owned
    // like the design size. The standing overrides are what
    // layout() honors between animations.
    bool animActive_ = false;
    bool animHasBox_ = false;
    bool animHasScale_ = false;
    Rect animFromBox_{}, animToBox_{};
    float animFromScale_ = 1.0f, animToScale_ = 1.0f;
    float animElapsed_ = 0.0f, animDuration_ = 0.0f;
    Easing animEasing_ = nullptr;
    float animSettledProgress_ = 0.0f;
    bool hasBoxOverride_ = false;
    Rect boxOverride_{};
    float scaleMul_ = 1.0f;
    Rect lastLayoutArg_{};
    bool hasLayoutArg_ = false;
    // Design-space placement state: the design inner at the
    // previous layout, and the children already placed in design
    // space. "Primed" is the WS_ABS_PRIMED state-word bit, the
    // same flag the absolute container uses (round-3 review
    // FRM-8/PERF-5: no parallel plain bool). "Fill" children -
    // primed without a fixed size onto the whole design rect -
    // are re-pinned to the design rect at every layout, so a
    // fill child's own drag/resize handling cannot displace it
    // out of the frame permanently (round-3 review FRM-3).
    Rect lastDesignInner_{};
    std::vector<Widget*> placed_;
    std::vector<Widget*> fillPlaced_;
    // The draw scratch list, retained across frames so its
    // capacity survives (round-3 review PERF-1; the TreeView
    // scratch_ precedent). Cleared per draw; mutable because
    // draw() is const.
    mutable DrawList scratch_;
    bool wasFillPlaced(const Widget* w) const {
        for (const Widget* p : fillPlaced_)
            if (p == w) return true;
        return false;
    }
    bool wasPlaced(const Widget* w) const {
        for (const Widget* p : placed_)
            if (p == w) return true;
        return false;
    }
};


// ---------------------------------------------------------- concrete set

class Label : public Frame {
public:
    Label(std::string id, std::string text);
    void setText(const std::string& t) {
        // Unchanged-text early-out (round-3 review PERF-8):
        // refresh paths call this per gesture with the same text.
        if (text_ != t) text_ = t;
    }
    const std::string& text() const { return text_; }
    // Rich text: when spans are set, they replace the plain text. A span
    // with no style inherits this label's effective text style.
    void setSpans(std::vector<TextSpan> spans) {
        spans_ = std::move(spans);
        mdLinks_.clear(); // hand-built spans carry no link mapping
    }
    // Markdown (src/markdown.h): parses `md` into fully resolved
    // spans against `base` and stores them like setSpans, plus the
    // link table behind onLinkClick. The single-argument form uses
    // this label's own textStyle override as the base when set,
    // else a default TextStyle. Resolution happens at call time:
    // restyle the label by calling again.
    // Defined in markdown.cpp (the parser lives there).
    void setMarkdown(const std::string& md);
    void setMarkdown(const std::string& md, const TextStyle& base);
    // Fired when a click lands on a markdown link span. Hit mapping
    // replays the same layout path draw uses, measured with the
    // metrics provider the last draw used (cached at draw time, the
    // lastStyle_ pattern; round-2 review MD-1), so the replay
    // agrees with what was actually drawn. Before the first draw it
    // falls back to the approximate provider.
    std::function<void(Label&, const std::string& url)> onLinkClick;
    bool handlePointer(const PointerEvent& e) override;
    float desiredHeight(const Style& style) const override;
    void layout(Rect r, const Style& style = defaultStyle()) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
private:
    std::string text_;
    std::vector<TextSpan> spans_;
    // Markdown link table: (span index into spans_, url). Set only
    // by setMarkdown; cleared by setSpans.
    std::vector<std::pair<std::size_t, std::string>> mdLinks_;
    // The metrics provider the last draw used, cached so the link
    // hit replay measures with the same advances the user saw
    // (review MD-1). Not owned; the context owns the provider.
    mutable const ITextMetrics* lastMetrics_ = nullptr;
};

class Panel : public Frame {
public:
    explicit Panel(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {}
protected:
    Color defaultBackground(const Style& s) const override { return s.panelBg; }
};

class Window; // TitleBar holds one; the full declaration follows it

// ------------------------------------------------------------- TitleBar
// A window's title bar as its own widget (build 14, JD directive:
// title bars "should be their own independent title bar widget
// with the Frame anchored to the top of the title bar so that
// everything scales properly"). A Window composes one TitleBar
// child across its top; the bar:
//   - owns the title text (the Window's title lives here now),
//     drawn from the theme's title text style, ellipsized to the
//     bar's inner width so it can never paint past the bar's own
//     box or straddle the window's outline (JD's gallery report:
//     "Litho GUI Controls" half cut at the window's top edge;
//     his inventory report: the title straddling the Frame's
//     outline). The title seats inside the bar's padded box: the
//     bar's textPad setting (build 19; the build-14 kInnerPad /
//     kTextInset constants were unified into it) bounds the
//     ellipsized width on every side, and the title anchors per
//     the bar's alignment setting - Center by JD's build-19
//     default, so window titles sit centered in their bars.
//   - owns the title-drag: pressing the bar and moving drags the
//     OWNER window's frame, exactly the move behavior windows
//     have always had (the drag state, WS_DRAGGING, still lives
//     on the owner; the bar drives it through the Window's
//     titleDrag* surface). The window's resize grips keep their
//     precedence inside the grip band: hit testing consults the
//     window's gripEdgesAt before descending into children, so a
//     grip press reaches the Window, never the bar.
// In a framed window the composition is: Frame (outer) -> Window
// -> TitleBar across the top, content beneath it; the Frame
// scales the whole anchored unit, title included, as one.
class TitleBar : public Frame {
public:
    explicit TitleBar(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {
        layoutExempt = true; // the owning Window places the bar
    }
    void setTitle(const std::string& t) { title_ = t; }
    const std::string& title() const { return title_; }
    // The bar's height under a Style (the theme's title bar
    // height); a Window lays its bar out at exactly this height.
    static float heightFor(const Style& s) { return s.titleBarHeight; }
    bool handlePointer(const PointerEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
private:
    friend class Window;
    void attachOwner(Window* w) { owner_ = w; }
    Window* owner_ = nullptr; // the Window this bar titles (not owned)
    std::string title_;
    float grabDx_ = 0.0f; // pointer offset from the owner's origin
    float grabDy_ = 0.0f;
};

class Window : public Frame {
public:
    Window(std::string id, std::string title);
    // The title lives on the composed TitleBar now (build 14);
    // these forward to it, so existing callers read and write
    // the same string they always did.
    const std::string& title() const { return titleBar_->title(); }
    void setTitle(const std::string& t) { titleBar_->setTitle(t); }
    TitleBar* titleBar() const { return titleBar_; }
    // The title bar's clearance inside the window's own outline
    // (build 22, JD's punch list item 1, from his Build 21
    // screenshot: the Inventory title bar "slightly on the trace
    // outline... shrink that on the left and right, five pixel
    // pad inside the frame and five pixel pad from the top, so
    // it's not touching anything"). The bar is laid out inset by
    // this much on the left, right, and top; its height stays the
    // theme's titleBarHeight, so content composed below the old
    // strip keeps its clearance (the strip's bottom moves down
    // by the pad, into the breath every scene already leaves).
    // First noted at build 13 and targeted at build 14, when the
    // bar still spanned the window edge to edge; build 22 seats
    // the BAR, not just its text, and pins the clearances.
    static constexpr float kTitleBarPad = 5.0f;
    // A window with an explicit frame keeps that frame when a parent (or
    // the context) lays it out; dragging the title bar updates the frame.
    void setFrame(Rect f) { frame_ = f; writeState(kHasFrameBit, true); }
    bool hasFrame() const { return hasState(kHasFrameBit); }
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    // The TitleBar's drag surface (build 14): the bar owns the
    // gesture, the window owns the state and the frame math,
    // exactly where they lived when the window painted and
    // dragged its own title.
    void titleDragStart() { raiseState(WS_DRAGGING); }
    void titleDragMoveTo(float x, float y) {
        Rect f = hasFrame() ? frame_ : rect_;
        f.x = x;
        f.y = y;
        setFrame(f);
        layout(f, lastStyle());
    }
    void titleDragEnd() { dropState(WS_DRAGGING); }
    bool titleDragging() const { return hasState(WS_DRAGGING); }
    // The resize edges (EDGE_* bits, private encoding) under a point
    // inside the window's outer grip band, or 0 when the point is not
    // in the band. This is the ONE definition of the grip band: the
    // window's own pointer handler starts resize drags from it, and
    // hit testing consults it to give the window's chrome precedence
    // over children inside the band (see UiContext / hit testing).
    int gripEdgesAt(float x, float y) const;

    // Resizing (JD requirement, 2026-11-01): a pointer down within
    // kResizeGrip logical pixels of any edge or corner (inside the
    // window rect) starts a resize drag instead of a title drag. The
    // frame follows the pointer live: the opposite edge stays planted,
    // corner drags move both axes, and content re-runs layout on every
    // move so anchored/box children reflow DURING the drag. The drag
    // raises WS_DRAGGING, like the title drag. A resize started on a
    // window without an explicit frame gives it one (the frame sticks).
    static constexpr float kResizeGrip = 6.0f;   // logical px, inside the rect
    // Minimum frame size in logical units. A FIXED floor, deliberately
    // not content-derived: content reflows while the floor is being
    // enforced, so a content-derived floor could shift mid-drag.
    static constexpr float kMinFrameW = 160.0f;
    static constexpr float kMinFrameH = 100.0f;
    // Fired on every move that actually changes the frame during a
    // resize drag; the payload is the new frame (logical units). The
    // last firing of a drag carries the final frame.
    std::function<void(Window&, Rect)> onResize;
protected:
    Color defaultBackground(const Style& s) const override { return s.windowBg; }
private:
    // Widget-specific state-word bit (allocation map in the WS_*
    // block): bit 58 of this window's own quad word (bit 32
    // before build 27; moved above Frame's range when Frame
    // entered every widget's ancestry).
    static constexpr std::uint64_t kHasFrameBit = 1ull << 58;
    // Resize edge bits (private: WS_*-style word, one drag at a time).
    static constexpr int EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8;
    TitleBar* titleBar_ = nullptr; // composed child, owned via children_
    Rect frame_{};
    int resizeEdges_ = 0;       // EDGE_* bits while a resize drag runs
    Rect dragStartFrame_{};     // frame when the resize drag began
    float dragStartX_ = 0.0f;   // pointer position when it began
    float dragStartY_ = 0.0f;
};


class Button : public Frame {
public:
    Button(std::string id, std::string label);
    const std::string& label() const { return label_; }
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(Button&)> onClick;
private:
    std::string label_;
    // "armed" (pointer down, click fires on release) is the WS_PRESSED bit.
};

class Toggle : public Frame {
public:
    Toggle(std::string id, std::string label, bool value = false);
    bool value() const { return hasState(WS_ON); }
    void setValue(bool v) { writeState(WS_ON, v); }
    const std::string& label() const { return label_; }
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(Toggle&, bool)> onChange;
    // The group's shared label column (build 22, JD's composition
    // law): when >= 0, the label starts this many units into the
    // padded box instead of immediately after the control glyph,
    // so a group mixing control widths (a 36-wide toggle track
    // beside 16-wide boxes) still shares ONE label line while
    // every control glyph shares the box's left edge. <0 = the
    // natural column (this control's own width + the 8 gap).
    // The same field, with the same meaning, lives on CheckBox
    // and RadioButton; a composition sets it uniformly per group.
    float labelColumnW = -1.0f;
private:
    std::string label_;
    void flip(); // the value itself is the WS_ON bit
};

class CheckBox : public Frame {
public:
    CheckBox(std::string id, std::string label, bool checked = false);
    bool checked() const { return hasState(WS_CHECKED); }
    void setChecked(bool v) { writeState(WS_CHECKED, v); }
    const std::string& label() const { return label_; }
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(CheckBox&, bool)> onChange;
    // The group's shared label column (build 22; see Toggle).
    float labelColumnW = -1.0f;
private:
    std::string label_;
    void flip(); // the check itself is the WS_CHECKED bit
};

// A radio button belongs to a named group; within the group (scoped to
// the button's parent container) exactly one button is selected at a time.
// select() enforces that by walking its group siblings.
class RadioButton : public Frame {
public:
    RadioButton(std::string id, std::string label, std::string groupId, int value);
    const std::string& groupId() const { return groupId_; }
    int value() const { return value_; }
    bool selected() const { return hasState(WS_SELECTED); }
    void setSelected(bool v) { writeState(WS_SELECTED, v); } // raw; prefer select()
    void select();
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(RadioButton&)> onSelect;
    // The group's shared label column (build 22; see Toggle).
    float labelColumnW = -1.0f;
private:
    std::string label_;
    std::string groupId_;
    int value_ = 0;
    // selection is the WS_SELECTED bit
};

struct TreeNode {
    int id = 0;
    std::string label;
    bool expanded = false;
    std::vector<TreeNode> children; // insertion order is display order
};

// A tree with virtualized row rendering: rows are the flattened
// expansion of the node tree (collapsed subtrees excluded), and draw
// + hit testing touch only the visible window of rows plus overscan,
// the VirtualList pattern applied to the flattened row array.
// The flatten is RE-DERIVED on every use: there is no cached row
// array, so the rows a draw or an event sees can never be stale
// against the node data. The derivation is window-only (post-review
// item 1, JD's design: re-flattened every use, optimized for speed
// and low calling overhead): one shared walk counts every visible
// row and stores only the caller's window into a member scratch
// buffer that is reused across calls, so steady-state draws and
// events allocate nothing and store O(window) rows however large
// the tree gets.
class TreeView : public Frame {
public:
    explicit TreeView(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {
        // Alignment class default (build 19): tree rows are one of
        // JD's two named exceptions to the center default - a
        // tree's rows seat Left. Instances may still choose.
        setTextAlign(TextAlign::Left);
    }
    std::vector<TreeNode>& roots() { return roots_; }
    const std::vector<TreeNode>& roots() const { return roots_; }
    int selectedId() const { return selectedId_; }
    // Programmatic selection (no intent fired); used by controllers
    // that keep the tree's selection in sync with their own state.
    void setSelectedId(int id) { selectedId_ = id; }
    bool focusable() const override { return true; }
    // Scroll offset in px over the flattened rows; wheel input moves
    // it, clamped to the content end.
    float scrollY() const { return scrollY_; }
    void setScrollY(float y) { scrollY_ = y; }
    // Flattened row count (expanded nodes only), counted fresh by
    // the shared walk on every call (stores nothing).
    int totalRowCount() const;
    int firstVisibleIndex() const;
    // Rows actually emitted by the last draw: the visible window plus
    // overscan. Bounded no matter how large the tree is.
    int drawnRowCount() const { return drawnRows_; }
    // Rows stored by the most recent flatten walk (test
    // instrumentation, like drawnRowCount()): the window size after
    // a draw, 0 after a count-only query. Proves the walk stored
    // the window, not the tree.
    int lastFlattenMaterialized() const { return lastMaterialized_; }
    // Row height refreshes in layout as well as draw (review
    // CORE-L4), so hit testing never uses a stale row height.
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    bool clipsChildren() const override { return true; } // draw PushClips rows
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(TreeView&, TreeNode&)> onToggle; // node expanded state changed
    std::function<void(TreeView&, TreeNode&)> onSelect;
private:
    struct Row { TreeNode* node; int depth; };
    // The one traversal behind totalRowCount(), firstVisibleIndex(),
    // pointer handling and draw. Counts every visible row; rows in
    // [winFirst, winLast) are appended to scratch_. When findIndex
    // >= 0 the walk stops as soon as that row is reached and, if it
    // exists, *foundOut receives it (foundOut->node stays null when
    // the index is past the end). Returns the visible row count
    // (a partial count when an early find stopped the walk; callers
    // that need the total pass findIndex < 0). Plain recursion over
    // the node data: no virtual calls, no callbacks, no per-node
    // allocation.
    int flattenWindow(int winFirst, int winLast, int findIndex, Row* foundOut) const;
    void walkNodes(std::vector<TreeNode>& nodes, int depth, int winFirst, int winLast,
                   int findIndex, int& count, Row* foundOut, bool& found) const;
    std::vector<TreeNode> roots_;
    int selectedId_ = -1;
    float rowH_ = 22.0f; // refreshed from Style on draw
    float scrollY_ = 0.0f;
    mutable int drawnRows_ = 0;
    // Reused flatten storage: cleared per walk (capacity retained),
    // so after a one-time growth to the window size, steady-state
    // walks allocate nothing.
    mutable std::vector<Row> scratch_;
    mutable int lastMaterialized_ = 0;
};

// A flat item list with virtualized row rendering, like TreeView:
// only the visible window of rows (plus overscan) is drawn or
// hit-tested, however many items the list holds.
class ListBox : public Frame {
public:
    explicit ListBox(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {
        // Alignment class default (build 19): a list reads as a
        // tree-adjacent enumeration - rows are data, scanned from
        // the left like tree rows - so ListBox takes the tree
        // family's Left default rather than the center default.
        // (JD named tree views and expandables as the exceptions;
        // lists are decided with them, and the decision is stated
        // in the build 19 record.) Instances may still choose.
        setTextAlign(TextAlign::Left);
    }
    std::vector<std::string>& items() { return items_; }
    int selectedIndex() const { return selected_; }
    void setSelectedIndex(int i) { selected_ = i; }
    bool focusable() const override { return true; }
    float scrollY() const { return scrollY_; }
    // Programmatic scroll: clamped into the scrollable range
    // (build 23; the draw already clamped its local copy, so a
    // stale offset could sit in the member past the end of a
    // shrunk list and firstVisibleIndex would believe it).
    void setScrollY(float y);
    int firstVisibleIndex() const;
    // Rows actually emitted by the last draw (visible window +
    // overscan): bounded no matter how many items exist.
    int drawnRowCount() const { return drawnRows_; }
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    bool clipsChildren() const override { return true; } // draw PushClips rows
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(ListBox&, int)> onSelect;
    // The scrollbar strip's width when the list overflows
    // (build 23): the same 8-unit overlay the scrollable Frame
    // uses, at the list's right inside edge, drawn only while
    // the content overflows. Exposed so a composition (the
    // ComboBox menu) and the tests can speak about the strip
    // without duplicating the number.
    static constexpr float kScrollbarW = 8.0f;
private:
    float maxScroll() const;
    // The scrollbar thumb's placement within the visible box:
    // its top (relative to the box's top) and its height, the
    // scrollable Frame's metrics in miniature.
    void scrollThumbMetrics(float& top, float& h) const;
    bool scrollbarHit(float x) const {
        return maxScroll() > 0.0f && x >= rect_.x + rect_.w - kScrollbarW &&
               x < rect_.x + rect_.w;
    }
    std::vector<std::string> items_;
    int selected_ = -1;
    float rowH_ = 22.0f;
    float scrollY_ = 0.0f;
    mutable int drawnRows_ = 0;
    // Drag-scroll gesture state (build 23): a press remembers
    // where it landed and the offset it landed on; a move past
    // the threshold becomes a scroll, not a click, and the
    // release then selects nothing. The scrollbar thumb drag
    // is the same gesture with the grab point held on the
    // thumb. Plain members, UI-thread owned like every other
    // gesture's state in this library.
    bool pressActive_ = false;
    bool pressScrollbar_ = false;
    bool dragScrolled_ = false;
    float pressY_ = 0.0f;
    float pressScrollY_ = 0.0f;
    float thumbGrabDy_ = 0.0f;
};

class ScrollArea : public Frame {
public:
    explicit ScrollArea(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {}
    float scrollY() const { return scrollY_; }
    void setScrollY(float y) { scrollY_ = y; }
    // Windowed mode (uniform-height rows only): when windowed mode
    // is set and the content child is a Column container, the
    // content's children are treated as uniform rows of
    // `windowRowHeight` px (Style::rowHeight when <= 0). Only the
    // visible window of rows (plus overscan) is laid out and left
    // visible; off-window rows are set invisible, which removes them
    // from draw and hit testing as well. The content child's own
    // padding is honored; its box layout is NOT run (that is the
    // point of the mode). Variable row heights still need the
    // full-layout path: leave windowed mode off and the area lays
    // out its full content as before. The mode flag is packed into
    // this area's quad word (bit 32, widget-specific range).
    bool windowedMode() const { return hasState(kWindowedBit); }
    void setWindowed(bool v) { writeState(kWindowedBit, v); }
    float windowRowHeight = 0.0f;
    // Rows left visible by the last windowed layout (0 when the
    // windowed path did not run).
    int windowedVisibleCount() const { return windowedVisible_; }
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    bool clipsChildren() const override { return true; } // draw PushClips content
    void draw(DrawList& dl, const DrawParams& params) const override;
private:
    // Widget-specific state-word bit: windowed mode (bit 58;
    // bit 32 before build 27, moved above Frame's range).
    static constexpr std::uint64_t kWindowedBit = 1ull << 58;
    float scrollY_ = 0.0f;
    float contentH_ = 0.0f;
    int windowedVisible_ = 0;
};

// A section with a header row: clicking the header toggles expansion.
// COMPOSED (build 26, JD's rule from his Build 23 gallery
// screenshots): "Every collapsible container should contain a
// frame that is fully hidden by its title bar and only expanded
// when the collapsible becomes expanded." The widget owns a
// title bar plus an inner Frame (id + "-body", elastic,
// owner-placed like a Window's TitleBar) holding a Column box
// (id + "-body-box"); content added to the Collapsible is
// adopted into the box at layout. Collapsed, the body frame is
// invisible and contributes NO height: the collapsible's total
// height is its title bar only, with no reserved content space
// (build 25 and earlier stacked content under the header inside
// the collapsible's own rect, so a caller-pinned fixedH left a
// collapsed section reserving dead space, and a too-small pin
// clipped an expanded section's content at its own bounds -
// both in JD's screenshots). Expanded, the body frame is
// visible and sized to its content, and the content sits inside
// it with bodyPad design units of padding at the top and bottom
// by default (between the title bar and the content, and
// between the content and the collapsible's bottom edge), so
// expanded content is never clipped by the collapsible's own
// bounds; an outer scrollable frame (build 20) still handles
// page-level overflow. bodyPad is a setting in this widget's
// own state-word range (bits 32..37, whole units 0..63,
// default 10 - JD: "there should be 10 pixels of padding
// between the bottom and top of every collapsible. By
// default."), settable per instance like every packed setting.
// The widget's own `padding` keeps its side role: it insets
// the body content left and right. fixedH no longer applies to
// this widget: its height derives from the title bar plus the
// padded content, which is the guarantee the rule states; a
// caller wanting a fixed-height region composes one around it
// (a Frame), as the gallery does.
class Collapsible : public Frame {
public:
    Collapsible(std::string id, std::string header, bool expanded = true);
    bool expanded() const { return hasState(WS_EXPANDED); }
    void setExpanded(bool v) { writeState(WS_EXPANDED, v); }
    const std::string& header() const { return header_; }
    // The composed parts (the Window::titleBar pattern): the
    // inner Frame, and the Column box inside it that holds the
    // adopted content.
    Frame* bodyFrame() const { return body_; }
    Panel* bodyBox() const { return bodyBox_; }
    // The body pad setting (see the class comment): whole
    // design units, quantized and clamped like setTextPad; a
    // non-finite value is refused and the last sane pad stands.
    static constexpr float kDefaultBodyPad = 10.0f;
    float bodyPad() const {
        return static_cast<float>(loadField(kBodyPadMask, kBodyPadShift));
    }
    void setBodyPad(float v) {
        if (!std::isfinite(v)) return;
        float q = std::floor(v + 0.5f);
        q = q < 0.0f ? 0.0f : (q > 63.0f ? 63.0f : q);
        storeField(kBodyPadMask, kBodyPadShift,
                   static_cast<std::uint64_t>(q));
    }
    float desiredHeight(const Style& style) const override;
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(Collapsible&, bool)> onToggle;
private:
    // Bits 58..63 since build 27 (was 32..37): above Frame's
    // range, per the hierarchy allocation rule.
    static constexpr int kBodyPadShift = 58;
    static constexpr std::uint64_t kBodyPadMask = 0x3Full << kBodyPadShift;
    float headerHeight(const Style& style) const {
        return style.rowHeight + 4.0f;
    }
    // The content's summed desired height (children's desired
    // heights plus their vertical margins), wherever the content
    // currently lives: direct children before the first layout
    // adopts them into the body box, the body box after.
    float contentDesiredHeight(const Style& style) const;
    // Moves any content added directly to the Collapsible into
    // the body box, preserving order. Runs at every layout, so
    // content added after the first layout is adopted too.
    void adoptContent();
    std::string header_;
    float headerH_ = 26.0f; // expansion is the WS_EXPANDED bit
    Frame* body_ = nullptr;    // composed child, owned via children_
    Panel* bodyBox_ = nullptr; // owned via body_
};

// UTF-8 text editor: text buffer + caret + selection, all in BYTE
// offsets that always sit on code point boundaries (movement,
// deletion and insertion operate on whole code points, never bytes).
// Character input arrives as KeyEvent.ch (one committed code point
// per event, any script, Systerian numerals included). Selection
// extends with shift+arrows/Home/End (KeyEvent.shift), with pointer
// press-drags, and by double click (PointerEvent.clickCount >= 2
// selects the word at the press).
//
// Clipboard is intent-based: the shell owns the clipboard. Copy/cut
// hand the selected text OUT through onCopy/onCut (ctrl+C / ctrl+X);
// paste fires onPasteRequest (ctrl+V) and the shell answers by
// calling insertText(). The field never touches a system clipboard.
//
// Two modes: single-line (Enter submits via onSubmit; newlines are
// stripped from inserts) and multi-line (setMultiLine: Enter inserts
// a newline, Up/Down move by logical line preserving the code point
// column, Home/End go to line bounds, and draw scrolls vertically so
// the caret line stays visible).
//
// IME composition is a shell-driven contract on the widget:
// imeBegin / imeUpdate carry a preedit string that draw renders
// inline at the caret with distinct styling (trace-purple run +
// underline, WS_COMPOSING raised); imeCommit(finalText) inserts the
// committed text (the current preedit when finalText is empty) and
// imeCancel drops it. The buffer itself is untouched until commit.
// While WS_COMPOSING is raised, buffer-mutating keys (character
// input, Backspace, Delete, Enter) do NOT touch the real buffer:
// composition keys belong to the shell's IME, which drives the
// widget only through imeUpdate / imeCommit / imeCancel. Caret
// movement keys still work (they move the composition anchor).
class TextField : public Frame {
public:
    explicit TextField(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {}
    const std::string& text() const { return text_; }
    void setText(const std::string& t);
    // Caret position as a byte offset (always a code point boundary).
    std::size_t cursor() const { return cursor_; }
    // Selection as byte offsets [start, end); empty when start == end.
    bool hasSelection() const { return selAnchor_ != cursor_; }
    std::size_t selectionStart() const { return std::min(selAnchor_, cursor_); }
    std::size_t selectionEnd() const { return std::max(selAnchor_, cursor_); }
    std::string selectedText() const;
    void selectAll();
    // Inserts UTF-8 text at the caret, replacing any selection. The
    // typing path, the paste path and IME commit all land here. In
    // single-line mode '\n' and '\r' are stripped.
    void insertText(const std::string& utf8);
    // Multi-line mode is a packed flag: bit 32 of this field's own
    // quad word (widget-specific range; see the WS_* allocation map).
    void setMultiLine(bool v) { writeState(kMultiLineBit, v); }
    bool multiLine() const { return hasState(kMultiLineBit); }
    // IME composition contract (see the class comment).
    void imeBegin();
    void imeUpdate(const std::string& preedit);
    void imeCommit(const std::string& finalText);
    void imeCancel();
    bool composing() const { return hasState(WS_COMPOSING); }
    const std::string& preedit() const { return preedit_; }
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    // Only the multi-line draw PushClips (to the field's rect); the
    // single-line draw does not clip.
    bool clipsChildren() const override { return multiLine(); }
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(TextField&, const std::string&)> onSubmit;
    // Clipboard intents (shell owns the clipboard): copy/cut report
    // the selected text; paste requests text, which the shell then
    // delivers through insertText().
    std::function<void(TextField&, const std::string&)> onCopy;
    std::function<void(TextField&, const std::string&)> onCut;
    std::function<void(TextField&)> onPasteRequest;
private:
    void deleteSelection();
    // Snaps cursor_/selAnchor_ to code point boundaries and clamps
    // the display-window state (dispStart_, scrollLine_) against the
    // CURRENT buffer. Called at the end of every buffer mutator so a
    // shrunk buffer can never leave stale display state behind for
    // draw or pointer mapping to dereference (review CORE-H6).
    void clampScrollState();
    // Byte offset nearest to logical x, walking code points from the
    // display window start under the approximate metrics (the same
    // layout-time provider the rest of the base uses).
    std::size_t offsetFromX(float x) const;
    // Pointer mapping for both modes: multi-line resolves the line
    // from y first (using the last draw's scroll offset), then x
    // within that line.
    std::size_t offsetFromPoint(float x, float y) const;
    // The same walks over an arbitrary buffer (the display buffer
    // during IME composition), plus the composition-aware entry the
    // pointer handler actually uses: it maps in display coordinates
    // and converts through the composition start before any result
    // touches the real buffer (review CORE-L6).
    // dispCursor is the caret expressed in the given buffer's
    // coordinates (the display buffer's, during composition):
    // the single-line window is computed around it (build 19).
    std::size_t offsetFromXBuf(const std::string& buf, float x, std::size_t dispCursor) const;
    std::size_t offsetFromPointBuf(const std::string& buf, float x, float y,
                                   std::size_t dispCursor) const;
    std::size_t mapPointer(float x, float y) const;
    // The single-line display window and text origin (build 19):
    // the visible slice of `disp` around `dispCursor` (the caret
    // in disp coordinates) under the field's padded box, plus the
    // x the slice's first code point draws at - the padded box's
    // left edge plus the alignment setting's anchor offset for
    // the slice's width (0 when the slice fills the box, so a
    // scrolled field left-anchors exactly as it always has).
    // Draw and the pointer mapping compute the SAME window from
    // the same inputs (each with its own metrics provider, the
    // standing draw-vs-pointer provider split), so the caret the
    // user sees is the caret a click maps to.
    void singleLineWindow(const std::string& disp, std::size_t dispCursor,
                          const TextStyle& ts, const ITextMetrics& m,
                          std::size_t& outStart, std::size_t& outEnd,
                          float& outOriginX) const;
    // The x a multi-line field's line [ls, le) of `buf` draws at:
    // the padded box's left edge plus the alignment offset of the
    // line's measured width. Shared by draw and pointer mapping.
    float lineOriginX(const std::string& buf, std::size_t ls, std::size_t le,
                      const TextStyle& ts, const ITextMetrics& m) const;
    void selectWordAt(std::size_t bytePos);
    // Logical-line helpers over '\n' (0x0A never occurs inside a
    // multi-byte sequence, so byte scans are boundary-safe).
    std::size_t lineStartOffset(std::size_t bytePos) const;
    std::size_t lineEndOffset(std::size_t bytePos) const;
    void moveVertical(int dy, bool extend);
    // Widget-specific state-word bit: multi-line mode (bit 58;
    // bit 32 before build 27, moved above Frame's range).
    static constexpr std::uint64_t kMultiLineBit = 1ull << 58;
    std::string text_;          // UTF-8 bytes
    std::size_t cursor_ = 0;    // byte offset, code point boundary
    std::size_t selAnchor_ = 0; // byte offset; == cursor_ = no selection
    std::string preedit_; // live IME composition string (not in text_)
    // Display window state from the last draw: the byte offset the
    // visible slice starts at. Pointer mapping walks from here.
    mutable std::size_t dispStart_ = 0;
    mutable int scrollLine_ = 0; // first visible line (multi-line draw)
};

// ------------------------------------------------- batch 2 (2026-11-01)

// Horizontal slider: track + thumb. Pointer drag captures the thumb;
// arrow keys step when focused. The value lives in an atomic float: it
// is the one numeric property a worker thread may legitimately poke
// (e.g. a seek position), and reads/writes stay lock-free.
class Slider : public Frame {
public:
    Slider(std::string id, float minValue = 0.0f, float maxValue = 1.0f,
           float value = 0.0f, float step = 0.0f);
    float minValue() const { return min_; }
    float maxValue() const { return max_; }
    float step() const { return step_; }
    float value() const { return value_.load(std::memory_order_relaxed); }
    // Programmatic set: clamps and snaps to step; does not fire intents.
    void setValue(float v);
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(Slider&, float)> onChange; // value changed (drag/key/set via input)
    std::function<void(Slider&, float)> onCommit; // drag released with a final value
private:
    float clampSnap(float v) const;
    float valueFromX(float x) const;
    void applyInput(float v); // input-driven set: fires onChange on change
    float min_ = 0.0f;
    float max_ = 1.0f;
    float step_ = 0.0f; // <= 0 = continuous
    std::atomic<float> value_{0.0f};
    // "dragging" is the WS_DRAGGING bit.
};

// Tab strip + content area showing exactly one page. Pages are ordinary
// children added with addTab; inactive pages get zero rects in layout
// and are not drawn (same trick as Collapsible). The active tab is a
// QUAD WORD of its own: activeMask_ holds exactly one bit, bit i for
// tab i, so a tab switch is a two-bit XOR diff between snapshots.
class Tabs : public Frame {
public:
    explicit Tabs(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) { dir = LayoutDir::None; }
    // The active tab is one bit of a 64-bit mask, so a Tabs holds at
    // most 64 tabs. (The mask was a DWORD capping tabs at 32, review
    // CORE-L1; the state-packing pass widened it to a quad word and
    // raised the cap to 64.) addTab past the cap returns nullptr and
    // does NOT adopt the page.
    static constexpr std::size_t kMaxTabs = 64;
    // Adds a page under a strip label; returns the page. The first tab
    // added becomes active.
    Widget* addTab(const std::string& label, std::unique_ptr<Widget> page);
    std::size_t tabCount() const { return labels_.size(); }
    const std::vector<std::string>& labels() const { return labels_; }
    int activeIndex() const;
    std::uint64_t activeMask() const { return activeMask_.load(std::memory_order_relaxed); }
    // Programmatic switch: no intent fired.
    void setActiveIndex(int i);
    // User-intent switch: fires onSelect.
    void activate(int i);
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(Tabs&, int)> onSelect;
private:
    float stripHeight(const Style& style) const { return style.rowHeight + 6.0f; }
    // Tab segment widths from the given metrics; shared by layout, draw
    // and hit testing so they can never disagree.
    std::vector<float> tabWidths(const Style& style, const ITextMetrics& m) const;
    std::vector<std::string> labels_;
    std::atomic<std::uint64_t> activeMask_{0};
    ApproxTextMetrics approx_{}; // layout-time metrics until a provider is set
};

// A closed field showing the selected item; clicking opens a Menu popup
// (the concrete Popup machinery) holding the items. The menu is a
// COMPOSED panel (build 23, JD's ruling from his Build 22 Android
// test: an opened dropdown's text floated bare over the widgets
// beneath and was "really hard to read" - "drop downs should have
// a background, a frame that is scrollable, and solid and draws
// over top of other widgets temporarily showing the options
// inside"): the Popup holds a Frame carrying the theme's opaque
// panel fill and Trace border, and the ListBox of the items is
// homed inside that frame, clipped to it, scrolling behind its
// own scrollbar when the list overflows the capped panel.
// Selecting closes the menu and fires onSelect. The context pointer is
// attached once by the shell (same back-pointer pattern as Popup::ctx_);
// without it, clicks fire onDropDown so the shell can open the menu
// itself via openMenu(context).
class ComboBox : public Frame {
public:
    explicit ComboBox(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {}
    std::vector<std::string>& items() { return items_; }
    const std::vector<std::string>& items() const { return items_; }
    int selectedIndex() const { return selected_; }
    void setSelectedIndex(int i); // programmatic; no intent
    std::string selectedText() const;
    bool isOpen() const { return hasState(WS_OPEN); }
    // Attaching registers this box as a context client (round-2
    // review C3): if the context is destroyed first, it calls
    // detachContext() and ctx_ is dropped instead of dangling.
    // Destruction order contract: a shell that destroys its
    // UiContext before the widgets must either re-attach the box to
    // a live context or accept that clicks fall back to onDropDown.
    void attach(UiContext& ctx);
    // Unregisters from the attached context (see attach).
    ~ComboBox() override;
    void detachContext(UiContext* ctx) override {
        if (ctx_ == ctx) ctx_ = nullptr;
    }
    void openMenu(UiContext& ctx);
    void closeMenu();
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(ComboBox&, int)> onSelect;
    std::function<void(ComboBox&)> onDropDown; // click with no context attached
private:
    std::vector<std::string> items_;
    int selected_ = -1;
    UiContext* ctx_ = nullptr;
    std::unique_ptr<Popup> menu_;   // created on first open; this box owns it
    // The menu's composed panel frame (build 23): the Popup
    // holds a Frame, the Frame holds the ListBox - see
    // openMenu. Not owned separately; the Popup's child does
    // the owning, this is the wiring pointer.
    Frame* menuFrame_ = nullptr;
    ListBox* menuList_ = nullptr;
};

// Read-only progress: the caller supplies the fraction (0..1) per state
// snapshot; the bar holds no animation state. The fraction is an atomic
// float because progress is the classic worker-thread-updated property;
// updates stay lock-free and draw reads a snapshot.
class ProgressBar : public Frame {
public:
    explicit ProgressBar(std::string id, float fraction = 0.0f);
    float fraction() const { return fraction_.load(std::memory_order_relaxed); }
    void setFraction(float f);
    // Draw a "NN%" TextRun centered on the bar. Packed into this
    // bar's quad word (bit 32, widget-specific range).
    bool showPercent() const { return hasState(kShowPercentBit); }
    void setShowPercent(bool v) { writeState(kShowPercentBit, v); }
    float desiredHeight(const Style& style) const override;
    void draw(DrawList& dl, const DrawParams& params) const override;
private:
    static constexpr std::uint64_t kShowPercentBit = 1ull << 58; // (bit 32 before build 27)
    std::atomic<float> fraction_{0.0f};
};

// ------------------------------------------------------------- animation
// The house easing set: pure functions of a normalized t (clamped
// to [0, 1] inside), shared by every animated control. Animation
// state advances ONLY through explicit tick(dt) calls from the
// shell; nothing in the GUI reads a clock, so a recorded sequence
// of ticks replays identically.
namespace anim {
float linear(float t);
float easeInOutCubic(float t);
float easeOutCubic(float t);
} // namespace anim

// ------------------------------------------------------------ BootWindow
// An animated loading window (Ready Player One HUD boot as the
// visual reference): a small centered window with a status line
// ("INITIALIZING"), a progress bar and a percent readout. When
// progress reaches 100% the stage flips to Complete (status shows
// the ready text), holds briefly, then the window EXPANDS from the
// target's center out to the full target rect while its content
// fades in, and finally reports Done through onBootComplete.
//
// Stages: Loading -> Complete -> Expanding -> Done. The shell
// drives everything with tick(dtSeconds) once per frame and feeds
// progress with setProgress (or sets autoRate and lets tick advance
// it). layout()/draw() never advance the machine; tick re-lays the
// window out at the current frame using the last-used Style.
//
// Shell flow (UiStateMachine): the InitWindow state's root shows a
// BootWindow; the shell ticks it each frame; onBootComplete fires
// once, and the shell answers with switchTo(UiState::MainMenu) to
// swap in the menu root. reset() returns the window to Loading at
// 0% for reuse.
enum class BootStage { Loading, Complete, Expanding, Done };

class BootWindow : public Window {
public:
    explicit BootWindow(std::string id, std::string title = "LithoKraft");

    Label* statusLabel() const { return status_; }
    Label* percentLabel() const { return percent_; }
    ProgressBar* progressBar() const { return bar_; }

    // The loading status line (default "INITIALIZING") and the text
    // shown during the Complete hold (default "COMPLETE").
    void setStatusText(const std::string& t);
    void setReadyText(const std::string& t) { readyText_ = t; }
    // Shell-driven progress, clamped to [0, 1], NaN-safe (via the
    // shared clampf). The stage leaves Loading only when tick sees
    // progress at exactly 1.0.
    void setProgress(float f);
    float progress() const { return progress_; }
    // Progress per second added by tick while Loading; 0 (default)
    // means the shell drives progress via setProgress.
    float autoRate = 0.0f;
    // Seconds spent in the Complete hold and in the expansion.
    float completeHold = 0.35f;
    float expansionDuration = 0.8f;

    // The full rect the window expands into. Default: the rect of
    // the first NON-DEGENERATE layout (a zero-area first layout
    // defers the capture instead of trapping a 0x0 target forever,
    // round-2 review C7).
    void setTargetRect(Rect r) {
        target_ = r;
        writeState(kHasTargetBit, true);
    }
    Rect targetRect() const { return target_; }
    bool hasTarget() const { return hasState(kHasTargetBit); }
    // The stage is a 2-bit FIELD inside this window's quad word
    // (bits 33..34, widget-specific range; see the WS_* allocation
    // map): stage() is (snapshot >> 33) & 3, transitions are two
    // RMWs by the shell's tick path, the field's single logical
    // owner (see Widget::storeField).
    BootStage stage() const {
        return static_cast<BootStage>(loadField(kStageMask, kStageShift));
    }
    // The small centered frame the window loads in (and expands
    // from), derived from the target rect.
    Rect startFrame() const;

    void tick(float dtSeconds);
    void reset();
    std::function<void(BootWindow&)> onBootComplete;

    // Before Done, layout drives the boot frame machine (start
    // frame, expansion interpolation, target). Once Done, layout
    // is a plain Window layout (round-2 review C8): the window
    // keeps whatever frame its drags left it with and never snaps
    // back to the target on re-layout.
    void layout(Rect r, const Style& style = defaultStyle()) override;
    // A loading gate: pointer input is swallowed until Done; then
    // the window behaves as a normal Window.
    bool handlePointer(const PointerEvent& e) override;
    void draw(DrawList& dl, const DrawParams& params) const override;

private:
    // Widget-specific state-word layout (allocation map in the WS_*
    // block): the stage as a 2-bit field at bits 59..60, has-target
    // at bit 61, complete-fired at bit 62, all in this window's own
    // quad word (33..34 / 35 / 36 before build 27). The field
    // starts at 59, not 58: bit 58 of this word is Window's
    // has-frame flag, and a BootWindow IS a Window. Bits 32..57
    // belong to Frame, in this word as in every widget's.
    static constexpr int kStageShift = 59; // (33 before build 27)
    static constexpr std::uint64_t kStageMask = 3ull << kStageShift;
    static constexpr std::uint64_t kHasTargetBit = 1ull << 61; // (35 before build 27)
    static constexpr std::uint64_t kCompleteFiredBit = 1ull << 62; // (36 before build 27)
    void setStage(BootStage s) {
        storeField(kStageMask, kStageShift, static_cast<std::uint64_t>(s));
    }
    bool completeFired() const { return hasState(kCompleteFiredBit); }
    void setCompleteFired(bool v) { writeState(kCompleteFiredBit, v); }

    Rect currentFrame() const;
    void refreshReadout();
    void relayout();

    Label* status_ = nullptr;
    Label* percent_ = nullptr;
    ProgressBar* bar_ = nullptr;
    std::string statusText_ = "INITIALIZING";
    std::string readyText_ = "COMPLETE";
    float progress_ = 0.0f;
    float holdT_ = 0.0f;
    float expandT_ = 0.0f;
    Rect target_{};
};

// ------------------------------------------------------- SysterianDecoder
// A decode-effect text widget (JD directive, 2026-11-02): a line of
// text that starts as pure SYSTERIAN - every character position
// shows a composite Systerian numeral glyph (the U+F0000+value
// code points of the font layer's mapping) - then decodes into its
// target ASCII, one character at a time, strictly left to right.
//
// While a position is still unlocked it FLICKERS: on each flicker
// step it rotates between a random Systerian numeral glyph and a
// random printable ASCII character, with a per-position phase
// offset so the line never blinks in unison. On the lock schedule
// the next position locks: from then on it shows its target
// character for good. Spaces in the target are spaces from the
// start (they carry no glyph) and do NOT consume lock steps.
//
// Time is shell-driven ONLY, exactly like BootWindow: the shell
// calls tick(dtSeconds) once per frame; layout()/draw() never
// advance the machine, and nothing reads a clock. The scramble is
// FRAME-RATE INDEPENDENT and reproducible by construction: the
// glyph/character shown at a position is a pure function of
// (seed, position, flicker step), hashed with a splitmix64-style
// mixer - there is no PRNG state to advance, so the same tick
// sequence always produces the same draw list, at any frame rate.
//
// Colors (theme-derived): scrambling Systerian in the Style's
// trace blue, scrambling ASCII in textMuted (dimmer), locked
// characters in the widget's full text color.
//
// The Systerian glyphs resolve through the ordinary fallback
// machinery: the widget appends the FontId given to
// setSysterianFont() to its runs' fallback chain (the shell
// registers the Systerian face and hands the id over, the same
// wiring the preview's font proof uses for its labels). With no
// id set, the Systerian runs carry no fallback and a back end
// without the glyph shows its honest placeholder.
//
// State: elapsed time and the decoded positions are plain fields
// (UI-thread data, like BootWindow's progress); the one boolean
// that matters - completion already reported - is bit 32 of this
// widget's own quad word (see the WS_* allocation map).
class SysterianDecoder : public Frame {
public:
    explicit SysterianDecoder(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {}

    // The final ASCII string the line decodes into. Setting it
    // resets the decode cleanly to t=0 (even mid-decode) and
    // clears the completion report, so the new line decodes from
    // pure Systerian again.
    void setText(const std::string& t);
    const std::string& text() const { return text_; }
    // The scramble seed. Fixed default (kDefaultSeed) so a bare
    // decoder is deterministic out of the box.
    void setSeed(std::uint64_t s) { seed_ = s; }
    std::uint64_t seed() const { return seed_; }
    static constexpr std::uint64_t kDefaultSeed = 0x9E3779B97F4A7C15ull;
    // decodeSpeed: how fast the decode runs. 1.0 is the normal
    // pace set by flickerInterval and lockInterval; 2.0 decodes
    // twice as fast, 0.5 half as fast. It scales the time fed
    // to the decoder, so the flicker and the left-to-right
    // locking speed up or slow down together. The setter keeps
    // the pace sane: NaN, zero or negative becomes 1.0 (back to
    // normal pace), and anything above 100.0 is clamped to
    // 100.0.
    void setDecodeSpeed(float v) {
        if (!(v > 0.0f)) v = 1.0f; // NaN / zero / negative
        decodeSpeed_ = v > 100.0f ? 100.0f : v;
    }
    float decodeSpeed() const { return decodeSpeed_; }
    // The registered Systerian face used for the numeral glyphs
    // (see the class comment). FONT_INVALID = not wired.
    void setSysterianFont(FontId f) { systerianFont_ = f; }
    FontId systerianFont() const { return systerianFont_; }

    // Seconds between flicker steps (the rotation rate of the
    // unlocked positions) and between character locks. Both must
    // be positive; a non-positive flicker interval freezes the
    // rotation on the all-Systerian first step, and a
    // non-positive lock interval locks everything on the first
    // tick that advances time.
    float flickerInterval = 0.05f;
    float lockInterval = 0.12f;

    // (Re)starts the decode from t=0; restart() is the same call
    // named for the after-completion call site. The decode also
    // runs on its own from construction / setText - start() is
    // only needed to run it again.
    void start();
    void restart() { start(); }
    // The ONLY time source. NaN / negative dt advances nothing
    // (the INV-3 guard form, as in BootWindow::tick). A huge dt
    // locks everything at once. The completion callback fires
    // from inside tick, exactly once per decode run. Each tick
    // advances the elapsed time by dt * decodeSpeed (the speed
    // scales the time at the tick boundary - see
    // setDecodeSpeed), so the scramble and the lock schedule
    // stay pure functions of the accrued elapsed time: at a
    // constant speed the decode is as deterministic and
    // tick-slicing-proof as it is at 1.0, and changing the
    // speed mid-decode simply changes how fast the elapsed
    // time accrues from that tick on.
    void tick(float dtSeconds);
    // Progress-drive (build 15): sets the accrued elapsed time
    // DIRECTLY instead of accruing it through tick - the entry
    // point for a driver that owns an external clock, like the
    // boot sequence, whose decoders are slaved to its progress
    // bar (see bootLogElapsedFor below). The decode state
    // becomes exactly the pure function of the driven value, so
    // driving to the same value always draws the same frame,
    // and a driver whose progress pauses holds the scramble
    // frozen mid-line. NaN / negative drives to 0. decodeSpeed
    // does NOT apply on this path (it scales tick accrual
    // only): the external clock is the whole pace. Fires
    // onDecodeComplete exactly once when the driven value
    // completes the decode; driving back below completion
    // un-locks characters again but does not re-arm the
    // callback - start() / setText() begin a new run.
    void setElapsed(float seconds);
    // True once every non-space position has locked. An empty
    // (or all-space) text is complete from the start; its
    // callback fires on the first tick.
    bool complete() const { return lockedCount() >= lockableCount_; }
    int lockedCount() const;   // non-space positions locked so far
    int lockableCount() const { return lockableCount_; }
    float elapsed() const { return elapsed_; }
    std::function<void(SysterianDecoder&)> onDecodeComplete;

    // The footprint is the TARGET string's, measured with the
    // widget's effective text style (approximate provider, the
    // Label wrap-path convention): it never changes as glyphs
    // flicker or lock, so parents never re-flow mid-decode.
    float desiredWidth(const Style& style) const override;
    float desiredHeight(const Style& style) const override;
    void draw(DrawList& dl, const DrawParams& params) const override;

private:
    // Widget-specific state-word bit (allocation map in the WS_*
    // block): bit 58 of this widget's own quad word (bit 32
    // before build 27; moved above Frame's range).
    static constexpr std::uint64_t kCompleteFiredBit = 1ull << 58;
    bool completeFired() const { return hasState(kCompleteFiredBit); }
    void setCompleteFired(bool v) { writeState(kCompleteFiredBit, v); }

    std::uint64_t flickerStep() const;
    // Whether position i shows a Systerian glyph (vs an ASCII
    // scramble character) at flicker step `step`.
    bool systShown(std::size_t i, std::uint64_t step) const;
    // The code point shown at unlocked position i at flicker step
    // `step`: a Systerian numeral or a printable ASCII character,
    // per systShown(), both hashed from (seed, i, step).
    char32_t shownCp(std::size_t i, std::uint64_t step) const;

    std::string text_;
    std::vector<char32_t> cps_;      // the target, one entry per position
    std::vector<int> lockIndexOf_;   // per position: -1 for a space,
                                     // else its ordinal among lockables
    int lockableCount_ = 0;
    float elapsed_ = 0.0f;
    float decodeSpeed_ = 1.0f;
    std::uint64_t seed_ = kDefaultSeed;
    FontId systerianFont_ = FONT_INVALID;
};

// ------------------------------------------------------- boot log drive
// The boot log's progress mapping (build 15, JD's boot
// directive): the boot sequence's text decodes UNDER its main
// progress bar - the bar is the master clock, and each log
// line's decode is a function of the bar's 0..1 progress, not
// of any timer of its own.
//
// The lines tile the progress in proportion to their lockable
// character counts: line i owns the segment
//   [ (sum of counts before i) / total, (that + count i) / total ]
// so the whole log decodes at a uniform characters-per-progress
// rate and line i+1 starts exactly where line i completes -
// the decoded fraction of the log tracks the bar. Given the
// bar's progress, this returns the accrued elapsed time to
// hand line `line`'s decoder via SysterianDecoder::setElapsed:
// 0 at its segment start (the line sits in its step-0 pure
// Systerian scramble), count * lockInterval at its segment end
// (the line stands fully decoded), linear between - so the
// line finishes exactly when the bar leaves its segment,
// never early, and at progress 1.0 every line is decoded.
// Progress outside [0, 1] clamps (NaN reads as 0); a line
// with no lockable characters, an out-of-range index, or a
// null table returns 0 (such a line is complete at any
// elapsed anyway). Pure and allocation-free: the same
// progress always yields the same elapsed, however the shell
// sliced its frames to reach it.
float bootLogElapsedFor(float progress, const int* lockableCounts,
                        std::size_t lineCount, std::size_t line,
                        float lockInterval);

// An image: emits one ImageQuad with a caller-supplied texture id.
// Texture contract: ids name RGBA textures owned by the back end;
// texel alpha is honored with straight-alpha source-over blending,
// and the command's imageAlpha (the widget's effective opacity:
// global uiOpacity times window and widget overrides) MULTIPLIES
// the texel alpha, so a PNG's alpha channel flows end to end and
// images fade with their window exactly like fills and text.
// imageAlpha is clamped to [0, 1] at emission (see DrawCmd). The
// GUI never decodes image files (PNG or otherwise): the shell
// decodes and registers textures and hands out ids. Layout size is
// the fixed size when set, else the natural size given by the
// caller, else zero. No input by default.
class Image : public Frame {
public:
    explicit Image(std::string id, int textureId = -1) : Frame(std::move(id), Frame::InertFrameTag{}), textureId_(textureId) {}
    int textureId() const { return textureId_; }
    void setTextureId(int t) { textureId_ = t; }
    void setNaturalSize(float w, float h) { naturalW_ = w; naturalH_ = h; }
    float desiredWidth(const Style& style) const override;
    float desiredHeight(const Style& style) const override;
    void draw(DrawList& dl, const DrawParams& params) const override;
private:
    int textureId_ = -1;
    float naturalW_ = -1.0f;
    float naturalH_ = -1.0f;
};

// Virtualized list: the clipper/recycling pattern for long lists (the
// Chat message list is the driving use case). The list owns NO row
// widgets and NO row data: the application reports a count and a label
// per index through callbacks, and the list keeps a small recycled pool
// of row bindings (index + label) covering only the visible window plus
// a fixed overscan. Scroll offsets map deterministically: row i sits at
// i * rowHeight - scrollY, in layout, draw and hit testing alike.
// One rich message row for VirtualList: an avatar image quad, an
// author line (bold, authorColor) and a message line built from rich
// text spans. Filled by VirtualList::richFn per row index.
struct RichRowData {
    int avatarTexture = 0;
    std::string author;
    Color authorColor{0, 0, 0, 0}; // alpha 0 = inherit the row style
    std::vector<TextSpan> spans;   // the message line (single line, clipped)
};

class VirtualList : public Frame {
public:
    explicit VirtualList(std::string id) : Frame(std::move(id), Frame::InertFrameTag{}) {
        // Alignment class default (build 19): the list family
        // (see ListBox) - rows seat Left.
        setTextAlign(TextAlign::Left);
    }
    // Data source callbacks (insertion-ordered, index-addressed).
    std::function<int()> countFn;
    std::function<std::string(int)> textFn;
    // Optional rich-row source: when set, rows render as RichRowData
    // (avatar + author + span line) at richRowH instead of the plain
    // single-run rows. textFn still fills the plain-text fallback.
    std::function<void(int, RichRowData&)> richFn;
    float richRowH = 46.0f;
    static constexpr int kOverscan = 2;
    int totalCount() const { return countFn ? countFn() : 0; }
    float scrollY() const { return scrollY_; }
    void setScrollY(float y) { scrollY_ = y; }
    int selectedIndex() const { return selected_; }
    void setSelectedIndex(int i) { selected_ = i; }
    // The currently bound window (after layout): first bound row index
    // (includes overscan) and how many rows are bound.
    int firstBoundIndex() const { return firstBound_; }
    int boundRowCount() const { return static_cast<int>(pool_.size()); }
    int firstVisibleIndex() const;
    bool focusable() const override { return true; }
    float desiredHeight(const Style& style) const override;
    void layout(Rect r, const Style& style = defaultStyle()) override;
    bool handlePointer(const PointerEvent& e) override;
    bool handleKey(const KeyEvent& e) override;
    bool clipsChildren() const override { return true; } // draw PushClips rows
    void draw(DrawList& dl, const DrawParams& params) const override;
    std::function<void(VirtualList&, int)> onSelect;
private:
    struct RowBinding { int index = -1; std::string text; RichRowData rich; };
    void rebind(const Style& style); // refill the pool for the current window
    float rowH() const { return rowH_; }
    float maxScroll() const;
    std::vector<RowBinding> pool_; // recycled bindings, window order
    int firstBound_ = 0;
    int selected_ = -1;
    float scrollY_ = 0.0f;
    float rowH_ = 22.0f; // refreshed from Style on layout/draw
};

// A popup is a widget tree of its own, managed by UiContext's popup stack
// rather than the main tree. Three flavors:
//   Modal:   captures all input while open; clicks outside are swallowed.
//   Menu:    closes on an outside click or on Escape; that click is swallowed.
//   Overlay: never interactive (tooltips); hit testing skips it, input
//            falls through, and it never swallows anything.
// Popups draw after the entire main tree, in stack order (later = above).
class Popup : public Frame {
public:
    enum class Flavor { Modal, Menu, Overlay };
    Popup(std::string id, Flavor flavor);
    // A popup destroyed while open unregisters itself from its
    // context's stack (review CORE-H1): the context never holds a
    // dangling popup pointer, and the context's focus/hover/capture
    // are repaired by pointer comparison only.
    ~Popup() override;
    Flavor flavor() const { return flavor_; }
    bool isOpen() const { return hasState(WS_OPEN); }
    // Frames are LOGICAL (widget-space) units, like every widget rect:
    // popup placement computed from widget rects needs no conversion.
    // Only the context boundary (layout viewport, pointer dispatch,
    // hit-test queries) speaks physical pixels; see UiContext.
    void setFrame(Rect f) { frame_ = f; writeState(kHasFrameBit, true); }
    bool hasFrame() const { return hasState(kHasFrameBit); }
    void close(); // asks the owning context to pop this popup
    void layout(Rect r, const Style& style = defaultStyle()) override;
    // Fired by the owning context whenever this popup is closed, by any
    // path (outside click, Escape, close()). Owners that mirror the open
    // state (ComboBox) hook this to keep their state word in sync.
    std::function<void(Popup&)> onClose;
private:
    friend class UiContext;
    void markOpen(bool v) { writeState(WS_OPEN, v); }
    // Widget-specific state-word bit (allocation map in the WS_*
    // block): bit 58 of this popup's own quad word (bit 32
    // before build 27; moved above Frame's range).
    static constexpr std::uint64_t kHasFrameBit = 1ull << 58;
    Flavor flavor_;
    Rect frame_{};
    UiContext* ctx_ = nullptr;
    // Focus to restore when this (modal) popup closes: the widget
    // that held focus when the popup opened (review CORE-H4).
    Widget* savedFocus_ = nullptr;
};

// A tooltip: a popup-style overlay the shell shows and hides. The hover
// DELAY is the shell's business (the GUI is clock-free); the shell calls
// show() with an anchor rect once its delay elapses and hide() when the
// hover ends. Drawn after the tree like every popup; Flavor::Overlay
// means it never captures or blocks input.
class Tooltip : public Popup {
public:
    Tooltip(std::string id, std::string text);
    const std::string& text() const { return label_->text(); }
    void setText(const std::string& t) { label_->setText(t); }
    // Sizes itself from the text (approximate metrics; exact shaping is
    // the font provider's side) and positions above the anchor, or
    // below it when there is no room above. Then opens on ctx. The
    // anchor is a TREE-space rect: callers inside a Frame must
    // pass the anchoring widget's treeRect(), not its rect()
    // (which is design coordinates there). Placement itself
    // never converts - the anchor arrives ready.
    void showAt(UiContext& ctx, Rect anchor);
    void hide() { close(); }
private:
    Label* label_ = nullptr;
};

// ---------------------------------------------------------------- context
// Owns interaction state for one widget tree: hit testing, pointer capture,
// focus, dispatch with bubbling, and the popup stack. One context per
// active UI state.
//
// Units contract (resolution independence): this boundary speaks
// PHYSICAL pixels. layout() takes the physical viewport, dispatchPointer
// takes physical pointer coordinates, hitTest() takes a physical point,
// and emitDraw() returns a physical draw list. The widget tree beneath
// works entirely in LOGICAL units (physical / Style::uiScale): widget
// rects, frames, anchors, and event coordinates inside handlers are all
// logical. Conversion happens exactly once per direction, here.
class UiContext {
public:
    // Detaches every open popup and every registered context client
    // (ComboBox) by nulling their back-pointers (round-2 review
    // C3): a popup or ComboBox that outlives its context never
    // dereferences it. MEMBER ORDER contract for owners (e.g.
    // cLithoGui): the context must be declared so it is destroyed
    // while the widgets that reference it are still the shell's
    // responsibility; detach makes any order SAFE, but a detached
    // popup's close() is a no-op and a detached ComboBox clicks fall
    // back to onDropDown until re-attached.
    ~UiContext();
    // Replaces the root tree. Any open popups are closed first (their
    // onClose fires), and focus/hover/capture are cleared by pointer
    // comparison only, never by dereferencing a widget from the old
    // tree (review CORE-H2). The old focus/hover widgets' state bits
    // cannot be cleared here for the same reason; they are recorded
    // and reaped lazily when the widget is next provably alive (in
    // emitDraw's tree walk), keeping applyFocus/applyHover's
    // "focus_ + WS_FOCUSED bits, kept in sync" contract true
    // (round-2 review C4).
    void setRoot(Widget* root);
    Widget* root() const { return root_; }

    // Stores a VALIDATED copy (see validatedStyle, review C6): a
    // Style set directly is held to the same safety floor as a
    // theme-supplied one.
    void setStyle(const Style& s) { style_ = validatedStyle(s); }
    const Style& style() const { return style_; }
    void setMetrics(const ITextMetrics* m) { metrics_ = m; }
    void setFonts(FontRegistry* fonts) { fonts_ = fonts; }
    FontRegistry* fonts() const { return fonts_; }
    // Binds the glyph source for the GlyphQuad expansion in emitDraw
    // (see IGlyphSource). Unbound (the default), draw lists carry
    // TextRuns only, exactly as before.
    void setGlyphSource(const IGlyphSource* src) { glyphSource_ = src; }
    const IGlyphSource* glyphSource() const { return glyphSource_; }

    // Lays the root into the PHYSICAL viewport (converted to the
    // logical viewport the tree actually lays out into).
    void layout(Rect viewport);
    // Deepest visible widget at the PHYSICAL point; children are tested
    // in reverse insertion order because the last drawn sibling is
    // topmost. Hit testing descends into a child whose own rect
    // contains the point even when an intermediate ancestor's rect
    // does not (overflow is hittable, review CORE-M7); drawing rules
    // are unchanged (no new clipping is implied).
    Widget* hitTest(float x, float y) const;
    bool dispatchPointer(const PointerEvent& e);
    bool dispatchKey(const KeyEvent& e);
    // Emits one frame into dl, scaled to PHYSICAL pixels (at uiScale
    // 1.0 the list is exactly the logical list, untouched). The list
    // is CLEARED at entry (review CORE-M3): one call is one frame.
    void emitDraw(DrawList& dl) const;
    Widget* focused() const { return focus_; }
    void setFocus(Widget* w) { applyFocus(w); }
    // Context-client registry (review C3): widgets holding a
    // back-pointer to this context (ComboBox) register on attach
    // and unregister on detach/destruction; ~UiContext detaches
    // whatever is still registered.
    void registerContextClient(Widget* w);
    void unregisterContextClient(Widget* w);

    // Popup stack. The context does not own popups; the caller keeps them
    // alive while they are open.
    void openPopup(Popup* p);
    void closePopup(Popup* p);
    void closeTopPopup();
    std::size_t popupCount() const { return popupStack_.size(); }

    // Window z-order. Windows are ordered among their parent's
    // children: later in insertion order draws on top and wins hit
    // testing (the tree's standing rule), so raising a window IS
    // moving it later among its siblings, and draw order + hit
    // testing follow with no second ordering structure to keep in
    // sync. raiseWindow moves the window to the top of its sibling
    // list; lowerWindow moves it to the bottom. A pointer down on any
    // widget inside a window raises that window automatically (see
    // dispatchPointer); these are the programmatic equivalents.
    // Popups are unaffected: they live on the popup stack and always
    // draw and route above every window. In an absolute
    // (LayoutDir::None) parent, reordering changes nothing but z:
    // frames and anchor state are per-child. In a box parent it also
    // changes flow order, which is the shell's choice to make.
    void raiseWindow(Window* w);
    void lowerWindow(Window* w);

private:
    const ITextMetrics& metrics() const;
    // The effective UI scale: Style::uiScale when positive and finite,
    // else 1.0. All boundary conversions go through this one helper.
    float scaleFactor() const;
    // Hit testing in LOGICAL coordinates (hitTest converts, dispatch
    // works logical after its own entry conversion).
    Widget* hitTestLogical(float x, float y) const;
    // The ancestor chain of `w`, target first, snapshotted BEFORE
    // any handler runs (review CORE-H5): dispatch walks the
    // snapshot, never a live parent pointer, so a handler that
    // destroys widgets cannot walk the dispatch out of a dead
    // widget. Shared by bubblePointer and dispatchKey (round-2
    // review C2).
    static std::vector<Widget*> snapshotChain(Widget* w);
    // True when `w` is reachable from the live root or an open
    // popup, comparing POINTERS ONLY (w itself is never
    // dereferenced), so it is safe to ask about a possibly-dead
    // widget (round-2 review C1).
    bool widgetAlive(const Widget* w) const;
    bool bubblePointer(Widget* target, const PointerEvent& e);
    void applyFocus(Widget* w); // focus_ + WS_FOCUSED bits, kept in sync
    void applyHover(Widget* w); // hover_ + WS_HOVERED bits, kept in sync
    // Restores a modal popup's saved focus when it is still valid
    // (used by closePopup and by forgetPopup, round-2 review C5).
    void restoreModalFocus(Popup* p);
    // Clears stale focus/hover state bits recorded by setRoot when
    // the walk reaches the (provably alive) widget holding them
    // (review C4).
    void reapStaleBits(Widget* w) const;
    // The topmost popup that takes part in input routing, or nullptr.
    // Overlay popups (tooltips) are skipped: they never interact.
    Popup* topInteractivePopup() const;
    // Removes a popup from the stack WITHOUT firing onClose and
    // repairs focus/hover/capture by pointer comparison. Used by
    // ~Popup (a dying popup unregisters itself) and by setRoot's
    // drain (which fires onClose separately, while the popup lives).
    void forgetPopup(Popup* p);
    // True when w is root or one of its descendants (pointer walks
    // only, no state touched).
    bool isInTree(const Widget* w, const Widget* root) const;
    // True when w may hold focus: in some live tree (root or an open
    // popup), with w and every ancestor visible and enabled.
    bool focusChainValid(const Widget* w) const;
    // Drops focus/hover/capture when they point at `dead` or inside
    // its subtree; pointers are compared, never dereferenced, except
    // for the ancestor walk over the (still living) dead tree.
    void repairInteractionPointers(const Widget* dead);
    friend class Popup;

    Widget* root_ = nullptr;
    Widget* focus_ = nullptr;
    Widget* hover_ = nullptr;
    Widget* capture_ = nullptr;
    // Focus/hover holders whose bits setRoot could not clear (the
    // old tree may have been dead): reaped by reapStaleBits when a
    // later tree walk proves them alive (review C4). Mutable
    // because the reap runs inside the const emitDraw.
    mutable Widget* staleFocusBit_ = nullptr;
    mutable Widget* staleHoverBit_ = nullptr;
    Style style_{};
    const ITextMetrics* metrics_ = nullptr;
    const IGlyphSource* glyphSource_ = nullptr;
    FontRegistry* fonts_ = nullptr;
    ApproxTextMetrics approx_{};
    std::vector<Popup*> popupStack_;
    std::vector<Widget*> contextClients_; // review C3 (ComboBox attach)
};

// ----------------------------------------------------------------- themes
// Themes are JSON files (parser: json.h). A theme applies over the
// built-in defaults token by token: any key that is missing or invalid
// keeps its default, and every problem is reported as a string in an
// error list instead of failing hard. A completely unparseable theme
// resets the style to defaults with one error entry.
//
// Shader slots: a theme may also bind USER SHADER files to stable slot
// ids. The base carries the binding (slot id -> path) and defines the
// uniform payload contract (see README); compiling and running shaders
// is a back-end job, never this layer's.
constexpr const char* SHADER_TRACE_BORDER = "trace-border"; // CustomDraw tag 1 pass
constexpr const char* SHADER_PANEL_FILL = "panel-fill";     // panel/window fill pass

struct ShaderBinding {
    std::string slot;
    std::string path;
};

class ThemeShaders {
public:
    void set(std::string slot, std::string path) {
        for (auto& b : bindings_) {
            if (b.slot == slot) { b.path = std::move(path); return; }
        }
        bindings_.push_back({std::move(slot), std::move(path)});
    }
    const std::string* find(const std::string& slot) const {
        for (auto& b : bindings_) if (b.slot == slot) return &b.path;
        return nullptr;
    }
    const std::vector<ShaderBinding>& bindings() const { return bindings_; }
    void clear() { bindings_.clear(); }

private:
    std::vector<ShaderBinding> bindings_; // insertion order, as in the file
};

// Parses `jsonText` and overlays the tokens it names onto `style` and
// `shaders` (overlay semantics: tokens the document does not name keep
// the values already in `style`; cLithoGui::applyThemeText therefore
// preserves shell-set values like uiScale across a theme load, review
// CORE-M2). Returns true when the document parsed (even if individual
// tokens were invalid and kept their defaults). On a document-level
// failure: resets `style` to defaults, clears `shaders`, appends one
// error, returns false. Numeric tokens are validated (review SUP-1):
// non-finite or out-of-range numbers are reported and keep the current
// value, never UB.
bool applyThemeJson(Style& style, ThemeShaders& shaders, const std::string& jsonText,
                    std::vector<std::string>& errors);

// ------------------------------------------------------------- ILithoGui
// The interface contract the engine and world talk to (interface family:
// IMod, IIntelOp, IHwLayer). cLithoGui is the in-house implementation in
// this base. Callers outside the GUI should depend on ILithoGui only.
class ILithoGui {
public:
    virtual ~ILithoGui() = default;
    // Replaces the root widget tree (the GUI does not own the tree).
    virtual void setRoot(Widget* root) = 0;
    virtual Widget* root() const = 0;
    virtual void setStyle(const Style& style) = 0;
    virtual const Style& style() const = 0;
    // Loads a JSON theme from any path (defaults + overlay; see
    // applyThemeJson). Errors are appended to `errors` when non-null.
    virtual bool loadThemeFromFile(const std::string& path,
                                   std::vector<std::string>* errors) = 0;
    virtual bool applyThemeJson(const std::string& json,
                                std::vector<std::string>* errors) = 0;
    virtual const ThemeShaders& shaders() const = 0;
    // Units contract: `viewport` and PointerEvent coordinates are
    // PHYSICAL pixels; the draw list emitFrame produces is physical
    // too. Widget rects and frames stay LOGICAL (physical /
    // Style::uiScale). See UiContext.
    virtual void layout(Rect viewport) = 0;
    virtual bool dispatchPointer(const PointerEvent& e) = 0;
    virtual bool dispatchKey(const KeyEvent& e) = 0;
    // Clears `dl` and emits one frame of draw commands (physical
    // pixels); tracePhase is the caller-supplied animation phase for
    // this frame.
    virtual void emitFrame(DrawList& dl, float tracePhase) = 0;
    virtual Widget* focused() const = 0;
    virtual UiContext& context() = 0;
};

class cLithoGui : public ILithoGui {
public:
    void setRoot(Widget* root) override { ctx_.setRoot(root); }
    Widget* root() const override { return ctx_.root(); }
    void setStyle(const Style& style) override { ctx_.setStyle(style); }
    const Style& style() const override { return ctx_.style(); }
    bool loadThemeFromFile(const std::string& path,
                           std::vector<std::string>* errors) override;
    bool applyThemeJson(const std::string& json,
                        std::vector<std::string>* errors) override;
    const ThemeShaders& shaders() const override { return shaders_; }
    void layout(Rect viewport) override { ctx_.layout(viewport); }
    bool dispatchPointer(const PointerEvent& e) override { return ctx_.dispatchPointer(e); }
    bool dispatchKey(const KeyEvent& e) override { return ctx_.dispatchKey(e); }
    void emitFrame(DrawList& dl, float tracePhase) override;
    Widget* focused() const override { return ctx_.focused(); }
    UiContext& context() override { return ctx_; }

private:
    bool applyThemeText(const std::string& json, std::vector<std::string>* errors);
    UiContext ctx_;
    ThemeShaders shaders_;
};

// ---------------------------------------------------------- boot flow states
// The boot flow skeleton: an initialization window shown while the engine
// warms up, then the main menu (with the world rendering behind it in the
// real shell; here the menu root is transparent by default).
enum class UiState { InitWindow, MainMenu };

class UiStateMachine {
public:
    UiStateMachine();
    UiState state() const { return state_; }
    void switchTo(UiState s);
    UiContext& context() { return ctx_; }
    Widget* rootFor(UiState s);
    Widget* initRoot() { return initRoot_.get(); }
    Widget* menuRoot() { return menuRoot_.get(); }

private:
    std::unique_ptr<Widget> buildInitTree();
    std::unique_ptr<Widget> buildMenuTree();

    UiContext ctx_;
    std::unique_ptr<Widget> initRoot_;
    std::unique_ptr<Widget> menuRoot_;
    UiState state_ = UiState::InitWindow;
};

} // namespace lithogui
