// Nemo View: the ASCII picture Little Nemo reads, built from the software renderer's own
// buffers (colour, 16-bit depth, the per-pixel object id and, when present, the face id).
// Integer math only, so the same view gives the same bytes on every machine and for every
// worker count.
//
// Each cell is a 64-bit quad word (nemo_cell.v1). The player decoder reads the whole word
// (glyph, ink, material fill, depth, alpha, light, face) so the picture can sit close to a
// solid, two-colour ASCII look: a dark material backing with a shaded character on top,
// faded by distance. Little Nemo's default prompt is still the glyph grid alone; the other
// fields are opt-in slices (depth, class, tex8, tex16, or the full word) for a later feed.
//
// Pipeline per cell (cells tile the source exactly; no float cell sizes):
//   reduce   RGB sum, nearest depth code, majority object id and face (ties go to the lower id)
//   quantize integer luma Y = (77R + 150G + 29B + 128) >> 8, depth band from a baked table,
//            nearest colour in a fixed 16-colour palette (ties go to the lower index)
//   dither   4x4 Bayer on the cell grid, luma' = clamp(Y + (bayer - 8) * 2)
//   glyph    12-step ramp " .:-=+*o#%@&", thinned by distance (fog per band); in Objects mode
//            walls, doors, keys, lights, pits, entities and liquid get their own characters
//   pack     fill, alpha, light and face into the quad word; reserved bits are zero
//
// Rows are independent: buildRows() writes only its own rows, so callers may split the rows
// across workers with no shared writes and join at their own phase boundary. finish() hashes
// the canonical record after the join. The old render.ascii look (software_ascii) is untouched.
//
// What Nemo may see: only the camera's own depth-tested pixels. The object id is the depth-test
// winner per pixel, so nothing behind a wall can appear. The record carries no position.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace litholattice::nemoview {

inline constexpr int kDepthBands = 8;   ///< 0 touching .. 6 far, 7 sky / nothing drawn
inline constexpr int kSkyBand = 7;
inline constexpr int kRampSteps = 12;
inline constexpr const char* kRamp = " .:-=+*o#%@&";  ///< ramp.v1, space first
inline constexpr int kPaletteSize = 16;
inline constexpr int kMaxObjectId = 63;

/// Object ids written into the software renderer's id buffer (from the block -> object table the
/// game supplies). 0 = nothing drawn (sky). Values above 15 are allowed and count as Other.
enum class ObjectId : uint8_t {
  Sky = 0,
  Floor = 1,
  Wall = 2,
  Pit = 3,
  Light = 4,
  Translucent = 5,
  Key = 6,
  Door = 7,
  Exit = 8,
  Entity = 9,
  Liquid = 10,
  Other = 11,
};

/// Software-renderer face ids (PackedVertex faceAo bits 0-2, same as kFaceNormals):
/// 0 +X, 1 -X, 2 +Y (top), 3 -Y (bottom), 4 +Z, 5 -Z. 7 = not known.
enum class FaceId : uint8_t {
  PosX = 0,
  NegX = 1,
  Top = 2,
  Bottom = 3,
  PosZ = 4,
  NegZ = 5,
  Unknown = 7,
};

/// Objects (default): object characters for walls, doors, keys and the rest, luma elsewhere.
/// PixelsAndDepth: pixels and depth only (no object ids), kept for the crutch comparison.
enum class ViewMode : uint8_t { Objects = 0, PixelsAndDepth = 1 };

/// How the player decoder paints a cell. Solid is the playable two-colour look (fill + ink).
enum class ViewStyle : uint8_t { Matrix = 0, TrueColour = 1, Depth = 2, Objects = 3, Solid = 4 };

const char* viewModeName(ViewMode m);
bool viewModeFromName(const std::string& name, ViewMode& out);
const char* viewStyleName(ViewStyle s);

/// A cell grid the ASCII renderer can be pointed at. The size lives here, as a
/// renderer / config option, not inside the Nemo View window, so the same
/// builder can later paint the main LithoKraft window as a human-playable
/// ASCII mode. That path is not wired yet; keep the window a consumer of the
/// finished frame and of this Grid.
///
/// Named selectable grids (each is its own hash format, beside 80x45):
///   80x45   hashing / record default, 320x180 capture, 4x4 cells
///   96x54   384x216 capture, 4x4 cells
///   160x90  display default, 320x180 capture, 2x2 cells
///   180x80  360x160 capture, 2x2 cells
///   240x135 480x270 capture, 2x2 cells
/// A later playable mode may use a different capture (for example 1920x1080
/// at 8x8 for 240x135) as long as the source tiles the grid exactly.
struct Grid {
  int cols = 80;
  int rows = 45;
  int captureW = 320;  ///< default Nemo / MCP capture for this grid; not the only legal source
  int captureH = 180;
  const char* name = "80x45";
};

inline constexpr int kSelectableGridCount = 5;
inline constexpr Grid kGrid80x45{80, 45, 320, 180, "80x45"};
inline constexpr Grid kGrid96x54{96, 54, 384, 216, "96x54"};
inline constexpr Grid kGrid160x90{160, 90, 320, 180, "160x90"};
inline constexpr Grid kGrid180x80{180, 80, 360, 160, "180x80"};
inline constexpr Grid kGrid240x135{240, 135, 480, 270, "240x135"};
/// Legacy MCP width, still accepted by vision.capture_ascii. Not a display choice.
inline constexpr Grid kGrid40x22{40, 22, 160, 88, "40x22"};
/// Fovea: centre 128x48 of an 80-col 320x180 capture, 32x12. Prompt budget unchanged.
inline constexpr Grid kFoveaGrid{32, 12, 128, 48, "32x12"};
inline constexpr Grid kHashGrid = kGrid80x45;
inline constexpr Grid kDisplayGrid = kGrid160x90;

const Grid* selectableGrids();  ///< kSelectableGridCount entries
const Grid& hashGrid();
const Grid& displayGrid();
/// Parse "160x90", "160", "display" / "default", "hash" / "record". Also accepts 40 and fovea / 32x12.
bool parseGrid(const std::string& spec, Grid& out);
/// Look up a known grid by columns (and rows when non-zero).
bool gridBySize(int cols, int rows, Grid& out);

/// Read-only view of one finished frame. Pixels are 0xAARRGGBB, rows top first.
struct SourceView {
  int width = 0, height = 0;
  const uint32_t* pixels = nullptr;
  const uint16_t* depth = nullptr;    ///< software depth codes (swsimd::depthQ); nullptr = all sky
  const uint8_t* objects = nullptr;   ///< per-pixel object id; nullptr = none
  const uint8_t* faces = nullptr;     ///< per-pixel face id 0-5; nullptr = unknown
  const uint16_t* layers = nullptr;  ///< per-pixel texture layer of the depth-test winner; display only
  const uint32_t* layerColors = nullptr;  ///< precomputed 0xFFRRGGBB per texture layer; display only
  int layerColorCount = 0;
  int depthRemapMode = 0;             ///< LL_SW_DEPTH_REMAP in effect when the frame was drawn (0..2)
  /// Optional crop (the fovea). Zero size = the whole frame.
  int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
};

struct FrameConfig {
  int cols = 80, rows = 45;  ///< hashing / record default (kHashGrid); not the display default
  ViewMode mode = ViewMode::Objects;
  static FrameConfig forGrid(const Grid& g, ViewMode m = ViewMode::Objects) {
    FrameConfig c;
    c.cols = g.cols;
    c.rows = g.rows;
    c.mode = m;
    return c;
  }
};

struct AsciiFrame {
  int cols = 0, rows = 0;
  ViewMode mode = ViewMode::Objects;
  uint64_t tick = 0;                  ///< display only; not part of the hashed record
  std::vector<uint64_t> cell;         ///< cols*rows packed nemo_cell.v1 words, row major
  std::string glyph;                  ///< derived: cols*rows characters, row major, no newlines
  std::vector<uint8_t> fg;            ///< derived: ink palette index 0..15 per cell
  std::vector<uint8_t> band;          ///< derived: depth band 0..7 per cell
  std::vector<uint8_t> object;        ///< derived: majority object id per cell (0 in PixelsAndDepth)
  std::vector<uint32_t> texColor;     ///< display only: averaged block-texture colour per cell (0xFFRRGGBB); not hashed
  std::array<uint8_t, 32> sha256{};   ///< SHA-256 of canonicalRecord(); set by finish()
};

// ---- nemo_cell.v1 bit layout (little-endian uint64) ----
//  0-7   glyph        ASCII character
//  8-11  ink          foreground palette 0-15
// 12-15  fill         background / material backing palette 0-15
// 16-19  depth        band 0-15 (v1 uses 0-7; 8-15 reserved for a finer table)
// 20-21  alpha        0 opaque, 1 glass, 2 liquid, 3 empty / sky
// 22-27  object       class 0-63
// 28-31  light        0-15 illumination
// 32-34  face         FaceId (7 = unknown)
// 35     edge         hard silhouette (0 in this cut)
// 36     emissive     lamp, torch, exit light
// 37     wet          0 in this cut
// 38-39  reserved_lo  must be 0
// 40-63  reserved     must be 0
inline constexpr int kCellGlyphShift = 0;
inline constexpr int kCellInkShift = 8;
inline constexpr int kCellFillShift = 12;
inline constexpr int kCellDepthShift = 16;
inline constexpr int kCellAlphaShift = 20;
inline constexpr int kCellObjectShift = 22;
inline constexpr int kCellLightShift = 28;
inline constexpr int kCellFaceShift = 32;
inline constexpr uint64_t kCellEdgeBit = 1ull << 35;
inline constexpr uint64_t kCellEmissiveBit = 1ull << 36;
inline constexpr uint64_t kCellWetBit = 1ull << 37;
inline constexpr uint64_t kCellReservedMask = 0xFFFFFFC000000000ull;  // bits 38-63

struct CellFields {
  char glyph = ' ';
  uint8_t ink = 0;      ///< 0-15
  uint8_t fill = 0;     ///< 0-15
  uint8_t depth = 7;    ///< 0-15, v1 0-7
  uint8_t alpha = 3;    ///< 0-3
  uint8_t object = 0;   ///< 0-63
  uint8_t light = 0;    ///< 0-15
  uint8_t face = 7;     ///< FaceId
  bool edge = false;
  bool emissive = false;
  bool wet = false;
};

uint64_t packCell(const CellFields& f);     ///< reserved bits forced to zero
CellFields unpackCell(uint64_t word);
inline uint64_t cellAt(const AsciiFrame& f, size_t i) {
  return i < f.cell.size() ? f.cell[i] : 0;
}

// ---- primitives (exposed for tests) ----
inline constexpr int luma8(int r, int g, int b) { return (77 * r + 150 * g + 29 * b + 128) >> 8; }
int depthBand(uint16_t code, int remapMode);
int bayer4(int cx, int cy);                          ///< 0..15
int paletteIndex(int r, int g, int b);               ///< nearest by squared distance, lower index wins ties
uint32_t paletteColour(int index);                   ///< 0xFFRRGGBB
char objectGlyph(ObjectId id);                       ///< 0 when the id has no character of its own
int fogSteps(int band);                              ///< ramp steps removed at this band
int fillScale(uint8_t face);                         ///< fill = ink * this / 256 (top lighter, bottom darker)
int fogFade(int band);                               ///< 0..256, how far colour is pulled toward the sky
const std::array<uint8_t, 32>& depthTableSha256();   ///< SHA-256 of the baked band table
std::string depthTableShaPrefix();                   ///< first 16 hex characters
/// Average of a fixed 4x4 sample grid over a square tile of 0xAARRGGBB texels (opaque only).
/// Deterministic; used to precompute per-block texture colours at load. Returns 0 if none.
uint32_t sampleTextureColor(const uint32_t* argb, int tile);
/// Same pattern over tightly packed RGBA8 bytes (R,G,B,A per pixel).
uint32_t sampleTextureColorRgba(const uint8_t* rgba, int tile);

// ---- building ----
/// Checks the geometry (exact tiling) and sizes `out`. False with `err` set on bad input.
bool prepare(const SourceView& src, const FrameConfig& cfg, AsciiFrame& out, std::string* err = nullptr);
/// Fills rows [rowBegin, rowEnd). Call after prepare(); safe to run on disjoint row ranges at once.
void buildRows(const SourceView& src, const FrameConfig& cfg, AsciiFrame& out, int rowBegin, int rowEnd);
/// Hashes the canonical record into out.sha256. Call once all rows are built.
void finish(AsciiFrame& out);
/// prepare + buildRows(all) + finish.
bool build(const SourceView& src, const FrameConfig& cfg, AsciiFrame& out, std::string* err = nullptr);

// ---- text forms ----
std::string glyphRows(const AsciiFrame& f);    ///< rows joined by '\n'
std::string fgRows(const AsciiFrame& f);       ///< one hex digit per cell (ink)
std::string bandRows(const AsciiFrame& f);     ///< one digit 0-7 per cell
std::string objectRows(const AsciiFrame& f);   ///< one base64url character per cell
/// Packed 8-bit slice: depth 0-7 (3 bits), alpha (2), light 0-7 (3), as two hex digits per cell.
std::string tex8Rows(const AsciiFrame& f);
/// Packed 16-bit slice: depth (4), alpha (2), object (6), light (4), as four hex digits per cell.
std::string tex16Rows(const AsciiFrame& f);
/// Full 64-bit word as 16 lowercase hex digits per cell (opt-in; never a default prompt).
std::string qwordRows(const AsciiFrame& f);
/// Named slice: "glyph" (default), "depth", "class", "fg", "tex8", "tex16", "qword". Empty if unknown.
std::string channelRows(const AsciiFrame& f, const std::string& channel);
/// nemo_ascii.v2|cols|rows|mode|ramp.v1|pal16.v1|lut| + raw little-endian cells.
std::string canonicalRecord(const AsciiFrame& f);
std::string shaHex(const AsciiFrame& f);

/// Lossless run-length form: a run of 4 or more of one character becomes <char><count>.
/// Newlines are kept and never run-length coded. Input must contain no digits.
std::string rleEncode(const std::string& text);
bool rleDecode(const std::string& rle, std::string& out);

// ---- player decoder (paint only; does not change the hashed word) ----

/// Paint-only look for a human reader. Never changes the hashed cell, the glyph
/// plane, or Nemo's prompt. The same settings can be used by the Nemo View
/// pop-up and by a later main-window ASCII play mode.
struct DisplaySettings {
  int gammaHundredths = 62;  ///< 50..100; 100 is linear. Default 62 (~0.62)
  int lumaFloor = 96;        ///< minimum foreground luma (0..255, ~38%). 0 = off. Sky and spaces are skipped.
  int contrast = 336;        ///< ink versus fill; 256 is unchanged, default about 1.31x
  int fogScale = 144;        ///< display fog as 256ths of the packed fade (256 = as packed). Default is gentler.
  int edgeLift = 80;         ///< extra luma on depth / object-id edges, display only
  bool textureColor = true;  ///< apply precomputed block-texture colours (paint only; default on)
};

inline constexpr DisplaySettings kDefaultDisplay{};
/// Hash-faithful paint: no gamma, no floor, packed fog, no edge lift, no texture colour.
inline constexpr DisplaySettings kRawDisplay{100, 0, 256, 256, 0, false};

struct CellLook {
  char glyph = ' ';
  uint32_t ink = 0xFF000000;   ///< 0xFFRRGGBB
  uint32_t fill = 0xFF000000;  ///< 0xFFRRGGBB; black when the style has no backing
};

CellLook decodeCell(uint64_t word, ViewStyle style, const DisplaySettings& display = kDefaultDisplay, uint32_t texColor = 0);
CellLook decodeCell(const AsciiFrame& f, size_t i, ViewStyle style, const DisplaySettings& display = kDefaultDisplay);

}  // namespace litholattice::nemoview
