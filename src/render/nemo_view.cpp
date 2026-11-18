// Nemo View: integer ASCII frame builder (see nemo_view.h).
#include "litholattice/render/nemo_view.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "litholattice/crypto/sha256.h"
#include "litholattice/render/nemo_view_depth_bands.h"

namespace litholattice::nemoview {
namespace {

// pal16.v1: the classic 16-colour VGA text palette.
constexpr uint32_t kPalette[kPaletteSize] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xC0C0C0,
    0x808080, 0x0000FF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF};

constexpr int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// Ramp steps removed per band: near cells keep full density, far cells thin toward '.'.
constexpr int kFog[kDepthBands] = {0, 0, 0, 1, 1, 2, 3, kRampSteps};

// How far the player decoder pulls ink and fill toward the sky, in 256ths.
constexpr int kFogFade[kDepthBands] = {0, 0, 16, 40, 72, 112, 168, 256};

// Material backing as a fraction of ink (256ths). Top stays fuller, bottom darker, sides mid.
constexpr int kFillScaleUnknown = 96;  // 3/8
constexpr int kFillScaleTop = 160;     // 5/8
constexpr int kFillScaleSide = 96;     // 3/8
constexpr int kFillScaleBottom = 64;   // 2/8

constexpr char kHex[] = "0123456789ABCDEF";
constexpr char kHexLo[] = "0123456789abcdef";
constexpr char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

uint32_t rgb(int r, int g, int b) {
  return 0xFF000000u | (uint32_t(clampi(r, 0, 255)) << 16) | (uint32_t(clampi(g, 0, 255)) << 8) |
         uint32_t(clampi(b, 0, 255));
}

uint32_t fadeTowardSky(uint32_t colour, int fade /* 0..256 */) {
  if (fade <= 0) return colour;
  if (fade >= 256) return 0xFF000000u;
  const int inv = 256 - fade;
  const int r = int((colour >> 16) & 0xFF) * inv / 256;
  const int g = int((colour >> 8) & 0xFF) * inv / 256;
  const int b = int(colour & 0xFF) * inv / 256;
  return rgb(r, g, b);
}

uint32_t scaleRgb(uint32_t colour, int scale /* 256 = identity */) {
  const int r = int((colour >> 16) & 0xFF) * scale / 256;
  const int g = int((colour >> 8) & 0xFF) * scale / 256;
  const int b = int(colour & 0xFF) * scale / 256;
  return rgb(r, g, b);
}

}  // namespace

const char* viewModeName(ViewMode m) { return m == ViewMode::PixelsAndDepth ? "pixels_depth" : "objects"; }

bool viewModeFromName(const std::string& name, ViewMode& out) {
  if (name == "objects") { out = ViewMode::Objects; return true; }
  if (name == "pixels_depth") { out = ViewMode::PixelsAndDepth; return true; }
  return false;
}

const char* viewStyleName(ViewStyle s) {
  switch (s) {
    case ViewStyle::TrueColour: return "true";
    case ViewStyle::Depth: return "depth";
    case ViewStyle::Objects: return "objects";
    case ViewStyle::Solid: return "solid";
    default: return "matrix";
  }
}

int depthBand(uint16_t code, int remapMode) {
  if (code == 0xFFFF) return kSkyBand;
  const int m = (remapMode >= 0 && remapMode <= 2) ? remapMode : 0;
  int b = 0;
  for (int k = 0; k < 6; ++k) b += code > kDepthBandEdges[m][k] ? 1 : 0;
  return b;
}

int bayer4(int cx, int cy) { return kBayer[cy & 3][cx & 3]; }

int paletteIndex(int r, int g, int b) {
  int best = 0;
  int bestD = 0x7FFFFFFF;
  for (int i = 0; i < kPaletteSize; ++i) {
    const int pr = int((kPalette[i] >> 16) & 0xFF), pg = int((kPalette[i] >> 8) & 0xFF), pb = int(kPalette[i] & 0xFF);
    const int d = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
    if (d < bestD) { bestD = d; best = i; }  // strict: the lower index keeps a tie
  }
  return best;
}

uint32_t paletteColour(int index) { return 0xFF000000u | kPalette[index & 15]; }

char objectGlyph(ObjectId id) {
  switch (id) {
    case ObjectId::Wall: return '#';
    case ObjectId::Door: return '|';
    case ObjectId::Key: return 'k';
    case ObjectId::Exit: return 'E';
    case ObjectId::Light: return '*';
    case ObjectId::Pit: return 'o';
    case ObjectId::Entity: return '&';
    case ObjectId::Liquid: return '~';
    default: return 0;
  }
}

int fogSteps(int band) { return kFog[clampi(band, 0, kDepthBands - 1)]; }

int fillScale(uint8_t face) {
  switch (FaceId(face)) {
    case FaceId::Top: return kFillScaleTop;
    case FaceId::Bottom: return kFillScaleBottom;
    case FaceId::PosX:
    case FaceId::NegX:
    case FaceId::PosZ:
    case FaceId::NegZ: return kFillScaleSide;
    default: return kFillScaleUnknown;
  }
}

int fogFade(int band) { return kFogFade[clampi(band, 0, kDepthBands - 1)]; }

uint64_t packCell(const CellFields& f) {
  uint64_t w = 0;
  w |= uint64_t(uint8_t(f.glyph)) << kCellGlyphShift;
  w |= uint64_t(f.ink & 15) << kCellInkShift;
  w |= uint64_t(f.fill & 15) << kCellFillShift;
  w |= uint64_t(f.depth & 15) << kCellDepthShift;
  w |= uint64_t(f.alpha & 3) << kCellAlphaShift;
  w |= uint64_t(f.object & 63) << kCellObjectShift;
  w |= uint64_t(f.light & 15) << kCellLightShift;
  w |= uint64_t(f.face & 7) << kCellFaceShift;
  if (f.edge) w |= kCellEdgeBit;
  if (f.emissive) w |= kCellEmissiveBit;
  if (f.wet) w |= kCellWetBit;
  w &= ~kCellReservedMask;  // reserved bits stay zero
  return w;
}

CellFields unpackCell(uint64_t word) {
  word &= ~kCellReservedMask;
  CellFields f;
  f.glyph = char(uint8_t((word >> kCellGlyphShift) & 0xFF));
  f.ink = uint8_t((word >> kCellInkShift) & 15);
  f.fill = uint8_t((word >> kCellFillShift) & 15);
  f.depth = uint8_t((word >> kCellDepthShift) & 15);
  f.alpha = uint8_t((word >> kCellAlphaShift) & 3);
  f.object = uint8_t((word >> kCellObjectShift) & 63);
  f.light = uint8_t((word >> kCellLightShift) & 15);
  f.face = uint8_t((word >> kCellFaceShift) & 7);
  f.edge = (word & kCellEdgeBit) != 0;
  f.emissive = (word & kCellEmissiveBit) != 0;
  f.wet = (word & kCellWetBit) != 0;
  return f;
}

const std::array<uint8_t, 32>& depthTableSha256() {
  static const std::array<uint8_t, 32> sha = [] {
    uint8_t bytes[3 * 6 * 2];
    int n = 0;
    for (int m = 0; m < 3; ++m)
      for (int k = 0; k < 6; ++k) {
        bytes[n++] = uint8_t(kDepthBandEdges[m][k] & 0xFF);
        bytes[n++] = uint8_t(kDepthBandEdges[m][k] >> 8);
      }
    return crypto::sha256(bytes, sizeof bytes);
  }();
  return sha;
}

std::string depthTableShaPrefix() {
  std::string s;
  for (int i = 0; i < 8; ++i) {
    s += char(std::tolower(kHex[depthTableSha256()[size_t(i)] >> 4]));
    s += char(std::tolower(kHex[depthTableSha256()[size_t(i)] & 15]));
  }
  return s;
}


namespace {

const Grid kSelectable[kSelectableGridCount] = {kGrid80x45, kGrid96x54, kGrid160x90, kGrid180x80, kGrid240x135};

std::string compactSpec(const std::string& spec) {
  std::string s;
  s.reserve(spec.size());
  for (unsigned char c : spec) {
    if (!std::isspace(c)) s.push_back(char(std::tolower(c)));
  }
  return s;
}

}  // namespace

const Grid* selectableGrids() { return kSelectable; }
const Grid& hashGrid() { return kHashGrid; }
const Grid& displayGrid() { return kDisplayGrid; }

bool gridBySize(int cols, int rows, Grid& out) {
  const Grid known[] = {kGrid80x45, kGrid96x54, kGrid160x90, kGrid180x80, kGrid240x135, kGrid40x22, kFoveaGrid};
  for (const Grid& g : known) {
    if (g.cols == cols && (rows == 0 || rows == g.rows)) {
      out = g;
      return true;
    }
  }
  return false;
}

bool parseGrid(const std::string& spec, Grid& out) {
  const std::string s = compactSpec(spec);
  if (s.empty()) return false;
  if (s == "display" || s == "default") {
    out = kDisplayGrid;
    return true;
  }
  if (s == "hash" || s == "record") {
    out = kHashGrid;
    return true;
  }
  if (s == "fovea") {
    out = kFoveaGrid;
    return true;
  }
  int cols = 0, rows = 0;
  const char* p = s.c_str();
  char* end = nullptr;
  cols = int(std::strtol(p, &end, 10));
  if (end == p || cols <= 0) return false;
  if (*end == 'x') {
    const char* r = end + 1;
    rows = int(std::strtol(r, &end, 10));
    if (end == r || *end != '\0' || rows <= 0) return false;
  } else if (*end != '\0') {
    return false;
  }
  return gridBySize(cols, rows, out);
}


uint32_t sampleTextureColor(const uint32_t* argb, int tile) {
  if (!argb || tile <= 0) return 0;
  uint64_t r = 0, g = 0, b = 0, n = 0;
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      const int x = (i * 2 + 1) * tile / 8;
      const int y = (j * 2 + 1) * tile / 8;
      const uint32_t t = argb[size_t(y) * size_t(tile) + size_t(x)];
      if ((t >> 24) < 128) continue;
      r += (t >> 16) & 0xFF;
      g += (t >> 8) & 0xFF;
      b += t & 0xFF;
      ++n;
    }
  if (!n) return 0;
  return 0xFF000000u | uint32_t(r / n) << 16 | uint32_t(g / n) << 8 | uint32_t(b / n);
}

uint32_t sampleTextureColorRgba(const uint8_t* rgba, int tile) {
  if (!rgba || tile <= 0) return 0;
  uint64_t r = 0, g = 0, b = 0, n = 0;
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      const int x = (i * 2 + 1) * tile / 8;
      const int y = (j * 2 + 1) * tile / 8;
      const uint8_t* p = rgba + (size_t(y) * size_t(tile) + size_t(x)) * 4;
      if (p[3] < 128) continue;
      r += p[0];
      g += p[1];
      b += p[2];
      ++n;
    }
  if (!n) return 0;
  return 0xFF000000u | uint32_t(r / n) << 16 | uint32_t(g / n) << 8 | uint32_t(b / n);
}

bool prepare(const SourceView& src, const FrameConfig& cfg, AsciiFrame& out, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  if (!src.pixels || src.width <= 0 || src.height <= 0) return fail("no source frame");
  if (cfg.cols <= 0 || cfg.rows <= 0 || cfg.cols > 512 || cfg.rows > 512) return fail("cols/rows out of range");
  const int cw = src.cropW > 0 ? src.cropW : src.width;
  const int ch = src.cropH > 0 ? src.cropH : src.height;
  if (src.cropX < 0 || src.cropY < 0 || src.cropX + cw > src.width || src.cropY + ch > src.height)
    return fail("crop outside the frame");
  if (cw % cfg.cols != 0 || ch % cfg.rows != 0) return fail("cells must tile the source exactly");
  const size_t n = size_t(cfg.cols) * size_t(cfg.rows);
  out.cols = cfg.cols;
  out.rows = cfg.rows;
  out.mode = cfg.mode;
  out.cell.assign(n, 0);
  out.glyph.assign(n, ' ');
  out.fg.assign(n, 0);
  out.band.assign(n, uint8_t(kSkyBand));
  out.object.assign(n, 0);
  out.texColor.assign(n, 0);  // display only; finish() / canonicalRecord() never read this
  out.sha256.fill(0);
  return true;
}

void buildRows(const SourceView& src, const FrameConfig& cfg, AsciiFrame& out, int rowBegin, int rowEnd) {
  const int cw = (src.cropW > 0 ? src.cropW : src.width) / cfg.cols;
  const int chh = (src.cropH > 0 ? src.cropH : src.height) / cfg.rows;
  const int cellPixels = cw * chh;
  const bool useObjects = cfg.mode == ViewMode::Objects && src.objects != nullptr;
  const bool useFaces = src.faces != nullptr;
  const bool useTex = src.layers != nullptr && src.layerColors != nullptr && src.layerColorCount > 0;
  rowBegin = std::max(rowBegin, 0);
  rowEnd = std::min(rowEnd, cfg.rows);
  for (int cy = rowBegin; cy < rowEnd; ++cy) {
    for (int cx = 0; cx < cfg.cols; ++cx) {
      uint32_t sr = 0, sg = 0, sb = 0;
      uint16_t nearest = 0xFFFF;
      uint16_t objCounts[16] = {};
      uint16_t faceCounts[8] = {};
      uint64_t tr = 0, tg = 0, tb = 0, tn = 0;
      const int px0 = src.cropX + cx * cw, py0 = src.cropY + cy * chh;
      for (int y = py0; y < py0 + chh; ++y) {
        const size_t row = size_t(y) * size_t(src.width);
        for (int x = px0; x < px0 + cw; ++x) {
          const uint32_t p = src.pixels[row + size_t(x)];
          sr += (p >> 16) & 0xFF;
          sg += (p >> 8) & 0xFF;
          sb += p & 0xFF;
          if (src.depth) nearest = std::min(nearest, src.depth[row + size_t(x)]);
          if (useObjects) {
            const uint8_t o = src.objects[row + size_t(x)];
            ++objCounts[o < 16 ? o : uint8_t(ObjectId::Other)];
          }
          if (useFaces) {
            const uint8_t face = src.faces[row + size_t(x)];
            ++faceCounts[face < 6 ? face : 7];
          }
          if (useTex && src.depth && src.depth[row + size_t(x)] != 0xFFFF) {
            const uint16_t layer = src.layers[row + size_t(x)];
            if (layer < uint16_t(src.layerColorCount)) {
              const uint32_t tc = src.layerColors[layer];
              if ((tc >> 24) >= 128) {
                tr += (tc >> 16) & 0xFF;
                tg += (tc >> 8) & 0xFF;
                tb += tc & 0xFF;
                ++tn;
              }
            }
          }
        }
      }
      const uint32_t half = uint32_t(cellPixels) / 2u;
      const int r = int((sr + half) / uint32_t(cellPixels));
      const int g = int((sg + half) / uint32_t(cellPixels));
      const int b = int((sb + half) / uint32_t(cellPixels));
      const int band = depthBand(nearest, src.depthRemapMode);
      int object = 0;
      if (useObjects) {
        uint16_t best = 0;
        for (int k = 0; k < 16; ++k)
          if (objCounts[k] > best) { best = objCounts[k]; object = k; }  // lower id keeps a tie
      }
      uint8_t face = uint8_t(FaceId::Unknown);
      if (useFaces) {
        uint16_t best = 0;
        for (int k = 0; k < 8; ++k)
          if (faceCounts[k] > best) { best = faceCounts[k]; face = uint8_t(k); }
      }

      CellFields cf;
      cf.depth = uint8_t(band);
      cf.object = uint8_t(object);
      cf.face = face;
      cf.ink = uint8_t(paletteIndex(r, g, b));
      const bool empty = (band == kSkyBand) || (useObjects && object == int(ObjectId::Sky));
      if (empty) {
        cf.alpha = 3;
        cf.glyph = ' ';
        cf.fill = 0;
        cf.ink = 0;
        cf.light = 0;
      } else {
        if (object == int(ObjectId::Liquid)) cf.alpha = 2;
        else if (object == int(ObjectId::Translucent)) cf.alpha = 1;
        else cf.alpha = 0;
        const int y8 = clampi(luma8(r, g, b) + (bayer4(cx, cy) - 8) * 2, 0, 255);
        int idx = (y8 * kRampSteps) >> 8;
        idx = std::max(1, idx - fogSteps(band));  // anything drawn shows at least '.'
        cf.glyph = kRamp[idx];
        if (useObjects) {
          const char og = objectGlyph(ObjectId(object));
          if (og) cf.glyph = og;
        }
        cf.light = uint8_t(y8 >> 4);
        if (object == int(ObjectId::Light) || object == int(ObjectId::Exit)) {
          cf.emissive = true;
          if (cf.light < 12) cf.light = 12;
        }
        const uint32_t ink = kPalette[cf.ink];
        const int scale = fillScale(cf.face);
        const int fr = int((ink >> 16) & 0xFF) * scale / 256;
        const int fgv = int((ink >> 8) & 0xFF) * scale / 256;
        const int fb = int(ink & 0xFF) * scale / 256;
        cf.fill = uint8_t(paletteIndex(fr, fgv, fb));
      }

      const uint64_t word = packCell(cf);
      const size_t i = size_t(cy) * size_t(cfg.cols) + size_t(cx);
      out.cell[i] = word;
      out.glyph[i] = cf.glyph;
      out.fg[i] = cf.ink;
      out.band[i] = uint8_t(band);
      out.object[i] = uint8_t(object);
      if (useTex && !empty && tn) {
        out.texColor[i] = 0xFF000000u | uint32_t((tr + tn / 2) / tn) << 16 |
                         uint32_t((tg + tn / 2) / tn) << 8 | uint32_t((tb + tn / 2) / tn);
      } else if (i < out.texColor.size()) {
        out.texColor[i] = 0;
      }
    }
  }
}

void finish(AsciiFrame& out) {
  const std::string rec = canonicalRecord(out);
  out.sha256 = crypto::sha256(reinterpret_cast<const uint8_t*>(rec.data()), rec.size());
}

bool build(const SourceView& src, const FrameConfig& cfg, AsciiFrame& out, std::string* err) {
  if (!prepare(src, cfg, out, err)) return false;
  buildRows(src, cfg, out, 0, cfg.rows);
  finish(out);
  return true;
}

namespace {
template <class F>
std::string rowsOf(const AsciiFrame& f, F cellChar) {
  std::string s;
  s.reserve(size_t(f.cols + 1) * size_t(f.rows));
  for (int y = 0; y < f.rows; ++y) {
    if (y) s += '\n';
    for (int x = 0; x < f.cols; ++x) s += cellChar(size_t(y) * size_t(f.cols) + size_t(x));
  }
  return s;
}

void appendHex(std::string& s, unsigned v, int digits) {
  for (int i = digits - 1; i >= 0; --i) s += kHexLo[(v >> (4 * i)) & 15];
}
}  // namespace

std::string glyphRows(const AsciiFrame& f) { return rowsOf(f, [&](size_t i) { return f.glyph[i]; }); }
std::string fgRows(const AsciiFrame& f) { return rowsOf(f, [&](size_t i) { return kHex[f.fg[i] & 15]; }); }
std::string bandRows(const AsciiFrame& f) { return rowsOf(f, [&](size_t i) { return char('0' + (f.band[i] & 7)); }); }
std::string objectRows(const AsciiFrame& f) { return rowsOf(f, [&](size_t i) { return kB64Url[f.object[i] & 63]; }); }

std::string tex8Rows(const AsciiFrame& f) {
  std::string s;
  s.reserve(size_t(f.cols * 2 + 1) * size_t(f.rows));
  for (int y = 0; y < f.rows; ++y) {
    if (y) s += '\n';
    for (int x = 0; x < f.cols; ++x) {
      const CellFields c = unpackCell(cellAt(f, size_t(y) * size_t(f.cols) + size_t(x)));
      const unsigned v = ((c.depth & 7) << 5) | ((c.alpha & 3) << 3) | ((c.light >> 1) & 7);
      appendHex(s, v, 2);
    }
  }
  return s;
}

std::string tex16Rows(const AsciiFrame& f) {
  std::string s;
  s.reserve(size_t(f.cols * 4 + 1) * size_t(f.rows));
  for (int y = 0; y < f.rows; ++y) {
    if (y) s += '\n';
    for (int x = 0; x < f.cols; ++x) {
      const CellFields c = unpackCell(cellAt(f, size_t(y) * size_t(f.cols) + size_t(x)));
      const unsigned v = (unsigned(c.depth & 15) << 12) | (unsigned(c.alpha & 3) << 10) |
                         (unsigned(c.object & 63) << 4) | unsigned(c.light & 15);
      appendHex(s, v, 4);
    }
  }
  return s;
}

std::string qwordRows(const AsciiFrame& f) {
  std::string s;
  s.reserve(size_t(f.cols * 16 + 1) * size_t(f.rows));
  for (int y = 0; y < f.rows; ++y) {
    if (y) s += '\n';
    for (int x = 0; x < f.cols; ++x) {
      uint64_t w = cellAt(f, size_t(y) * size_t(f.cols) + size_t(x));
      // Little-endian bytes as hex, so a later feed can recover the word with no extra convention.
      for (int b = 0; b < 8; ++b) {
        appendHex(s, unsigned(w & 0xFF), 2);
        w >>= 8;
      }
    }
  }
  return s;
}

std::string channelRows(const AsciiFrame& f, const std::string& channel) {
  if (channel == "glyph" || channel.empty()) return glyphRows(f);
  if (channel == "depth") return bandRows(f);
  if (channel == "class" || channel == "cls" || channel == "object") return objectRows(f);
  if (channel == "fg" || channel == "ink") return fgRows(f);
  if (channel == "tex8") return tex8Rows(f);
  if (channel == "tex16") return tex16Rows(f);
  if (channel == "qword" || channel == "cell") return qwordRows(f);
  return {};
}

std::string canonicalRecord(const AsciiFrame& f) {
  std::string s = "nemo_ascii.v2|" + std::to_string(f.cols) + "|" + std::to_string(f.rows) + "|" +
                  viewModeName(f.mode) + "|ramp.v1|pal16.v1|" + depthTableShaPrefix() + "|";
  const size_t n = size_t(f.cols) * size_t(f.rows);
  const size_t have = std::min(n, f.cell.size());
  s.resize(s.size() + have * 8);
  char* p = s.data() + (s.size() - have * 8);
  for (size_t i = 0; i < have; ++i) {
    uint64_t w = f.cell[i] & ~kCellReservedMask;
    for (int b = 0; b < 8; ++b) {
      p[i * 8 + b] = char(w & 0xFF);
      w >>= 8;
    }
  }
  return s;
}

std::string shaHex(const AsciiFrame& f) {
  std::string s;
  for (uint8_t v : f.sha256) {
    s += char(std::tolower(kHex[v >> 4]));
    s += char(std::tolower(kHex[v & 15]));
  }
  return s;
}

std::string rleEncode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    size_t j = i + 1;
    if (c != '\n')
      while (j < text.size() && text[j] == c) ++j;
    const size_t run = j - i;
    if (run >= 4) {
      out += c;
      out += std::to_string(run);
    } else {
      out.append(run, c);
    }
    i = j;
  }
  return out;
}

bool rleDecode(const std::string& rle, std::string& out) {
  out.clear();
  for (size_t i = 0; i < rle.size();) {
    const char c = rle[i];
    if (c >= '0' && c <= '9') return false;  // a count with no character before it
    size_t j = i + 1;
    size_t n = 0;
    while (j < rle.size() && rle[j] >= '0' && rle[j] <= '9') {
      n = n * 10 + size_t(rle[j] - '0');
      if (n > 1u << 20) return false;
      ++j;
    }
    if (j == i + 1) {
      out += c;
    } else {
      if (n < 4 || c == '\n') return false;  // not a form rleEncode makes
      out.append(n, c);
    }
    i = j;
  }
  return true;
}


// Display-only gamma 0.62: out = round(255 * (i/255)^0.62). Not used when packing cells.
constexpr uint8_t kGamma62[256] = {
      0,   8,  13,  16,  19,  22,  25,  27,  30,  32,  34,  36,  38,  40,  42,  44,
     46,  48,  49,  51,  53,  54,  56,  57,  59,  60,  62,  63,  65,  66,  68,  69,
     70,  72,  73,  74,  76,  77,  78,  80,  81,  82,  83,  85,  86,  87,  88,  89,
     91,  92,  93,  94,  95,  96,  97,  99, 100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123,
    124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 136, 137, 138,
    139, 140, 141, 142, 143, 144, 144, 145, 146, 147, 148, 149, 150, 151, 151, 152,
    153, 154, 155, 156, 156, 157, 158, 159, 160, 161, 161, 162, 163, 164, 165, 166,
    166, 167, 168, 169, 170, 170, 171, 172, 173, 173, 174, 175, 176, 177, 177, 178,
    179, 180, 180, 181, 182, 183, 184, 184, 185, 186, 187, 187, 188, 189, 190, 190,
    191, 192, 192, 193, 194, 195, 195, 196, 197, 198, 198, 199, 200, 200, 201, 202,
    203, 203, 204, 205, 205, 206, 207, 208, 208, 209, 210, 210, 211, 212, 212, 213,
    214, 215, 215, 216, 217, 217, 218, 219, 219, 220, 221, 221, 222, 223, 223, 224,
    225, 225, 226, 227, 227, 228, 229, 229, 230, 231, 231, 232, 233, 233, 234, 235,
    235, 236, 237, 237, 238, 239, 239, 240, 240, 241, 242, 242, 243, 244, 244, 245,
    246, 246, 247, 247, 248, 249, 249, 250, 251, 251, 252, 253, 253, 254, 254, 255
};

const uint8_t* gammaLut(int hundredths) {
  const int g = clampi(hundredths, 50, 100);
  if (g >= 100) return nullptr;  // linear
  if (g == 62) return kGamma62;
  // Other values: build a LUT once per setting (display only; not part of any hash).
  static uint8_t lut[256];
  static int cached = -1;
  if (cached == g) return lut;
  for (int i = 0; i < 256; ++i) {
    if (i == 0) {
      lut[i] = 0;
      continue;
    }
    // Integer power via 16-bit linear interpolation between the 0.62 table and identity.
    // g=50 is a bit brighter than 0.62, g=80 sits toward linear.
    const int t62 = kGamma62[i];
    if (g <= 62) {
      const int extra = (62 - g) * (t62 - i > 0 ? (t62 - i) : 0) / 12;  // 50 is 12 below 62
      lut[i] = uint8_t(clampi(t62 + extra, 0, 255));
    } else {
      lut[i] = uint8_t(t62 + (i - t62) * (g - 62) / 38);
    }
  }
  cached = g;
  return lut;
}

uint32_t applyLumaCurve(uint32_t colour, const uint8_t* lut) {
  if (!lut) return colour;
  const int r = int((colour >> 16) & 0xFF);
  const int g = int((colour >> 8) & 0xFF);
  const int b = int(colour & 0xFF);
  const int y = luma8(r, g, b);
  const int y2 = lut[y];
  if (y <= 0) return rgb(y2, y2, y2);
  return rgb((r * y2 + y / 2) / y, (g * y2 + y / 2) / y, (b * y2 + y / 2) / y);
}

uint32_t liftLumaFloor(uint32_t colour, int floor) {
  if (floor <= 0) return colour;
  const int r = int((colour >> 16) & 0xFF);
  const int g = int((colour >> 8) & 0xFF);
  const int b = int(colour & 0xFF);
  const int y = luma8(r, g, b);
  if (y >= floor) return colour;
  if (y <= 0) return rgb(floor, floor, floor);
  return rgb(clampi((r * floor + y / 2) / y, 0, 255), clampi((g * floor + y / 2) / y, 0, 255),
             clampi((b * floor + y / 2) / y, 0, 255));
}

uint32_t contrastAgainst(uint32_t ink, uint32_t fill, int contrast) {
  if (contrast == 256) return ink;
  const int ir = int((ink >> 16) & 0xFF), ig = int((ink >> 8) & 0xFF), ib = int(ink & 0xFF);
  const int fr = int((fill >> 16) & 0xFF), fg = int((fill >> 8) & 0xFF), fb = int(fill & 0xFF);
  return rgb(clampi(fr + (ir - fr) * contrast / 256, 0, 255),
             clampi(fg + (ig - fg) * contrast / 256, 0, 255),
             clampi(fb + (ib - fb) * contrast / 256, 0, 255));
}

uint32_t lightenInk(uint32_t colour, int lift) {
  if (lift <= 0) return colour;
  return rgb(clampi(int((colour >> 16) & 0xFF) + lift, 0, 255),
             clampi(int((colour >> 8) & 0xFF) + lift, 0, 255),
             clampi(int(colour & 0xFF) + lift, 0, 255));
}

int displayFogFade(int band, int fogScale) {
  const int b = band & 7;
  if (b == kSkyBand) return 256;
  return fogFade(b) * clampi(fogScale, 0, 256) / 256;
}

void applyDisplay(CellLook& look, const CellFields& c, const DisplaySettings& d, bool edge) {
  const bool sky = (c.depth & 7) == kSkyBand || c.alpha == 3;
  const bool blank = look.glyph == ' ' || sky;
  const uint8_t* lut = gammaLut(d.gammaHundredths);
  look.ink = applyLumaCurve(look.ink, lut);
  if ((look.fill & 0x00FFFFFFu) != 0) {
    // Fill gets a milder lift so the glyph stays brighter than its backing.
    look.fill = applyLumaCurve(look.fill, lut);
    look.ink = contrastAgainst(look.ink, look.fill, d.contrast);
  }
  if (!blank) look.ink = liftLumaFloor(look.ink, d.lumaFloor);
  if (edge && !sky) look.ink = lightenInk(look.ink, d.edgeLift);
}

bool neighbourEdge(const AsciiFrame& f, size_t i) {
  if (f.cols <= 0 || i >= f.cell.size()) return false;
  const CellFields c = unpackCell(f.cell[i]);
  if (c.edge) return true;
  const int x = int(i % size_t(f.cols));
  const int y = int(i / size_t(f.cols));
  const int dx[4] = {-1, 1, 0, 0};
  const int dy[4] = {0, 0, -1, 1};
  for (int k = 0; k < 4; ++k) {
    const int nx = x + dx[k], ny = y + dy[k];
    if (nx < 0 || ny < 0 || nx >= f.cols || ny >= f.rows) continue;
    const CellFields n = unpackCell(f.cell[size_t(ny) * size_t(f.cols) + size_t(nx)]);
    if (n.object != c.object || ((n.depth ^ c.depth) & 7) != 0) return true;
  }
  return false;
}

CellLook decodeCell(uint64_t word, ViewStyle style, const DisplaySettings& display, uint32_t texColor) {
  const CellFields c = unpackCell(word);
  CellLook look;
  look.glyph = c.glyph;
  look.fill = 0xFF000000u;
  look.ink = 0xFF000000u;
  switch (style) {
    case ViewStyle::TrueColour:
      look.ink = paletteColour(c.ink);
      break;
    case ViewStyle::Depth: {
      static const uint32_t kHeat[8] = {0xFFFF2828, 0xFFFF781E, 0xFFFFC828, 0xFFA0E63C,
                                        0xFF3CC8A0, 0xFF3C8CE6, 0xFF5050C8, 0xFF000000};
      look.ink = kHeat[c.depth & 7];
      break;
    }
    case ViewStyle::Objects: {
      switch (ObjectId(c.object)) {
        case ObjectId::Wall: look.ink = 0xFFAAAAB4; break;
        case ObjectId::Door: look.ink = 0xFF50DCFF; break;
        case ObjectId::Key: look.ink = 0xFFFFDC3C; break;
        case ObjectId::Light: look.ink = 0xFFFFFAC8; break;
        case ObjectId::Exit: look.ink = 0xFF78FF78; break;
        case ObjectId::Pit: look.ink = 0xFFDC3CDC; break;
        case ObjectId::Floor: look.ink = 0xFF468C46; break;
        case ObjectId::Translucent: look.ink = 0xFF96DCFF; break;
        case ObjectId::Liquid: look.ink = 0xFF3C78DC; break;
        default: look.ink = 0xFF787878; break;
      }
      break;
    }
    case ViewStyle::Solid: {
      // Two colours per cell: a darker material backing and a glyph in the surface ink,
      // faded toward the sky by depth. Ink keeps the framebuffer's lighting; face only
      // changes how full the backing is (already packed into fill).
      look.ink = paletteColour(c.ink);
      look.fill = paletteColour(c.fill);
      if (display.textureColor && (texColor & 0x00FFFFFFu) != 0) {
        // Texture albedo, dimmed by the cell's packed shade (light 0..15).
        const int shade = clampi((c.light & 15) * 17, 0, 255);
        look.ink = scaleRgb(texColor, shade);
        look.fill = scaleRgb(texColor, shade * fillScale(c.face) / 256);
      }
      const int fade = displayFogFade(c.depth, display.fogScale);
      look.ink = fadeTowardSky(look.ink, fade);
      look.fill = fadeTowardSky(look.fill, fade);
      if (c.emissive) {
        look.ink = scaleRgb(look.ink, 256 + c.light * 2);
        look.fill = scaleRgb(look.fill, 256 + c.light);
      }
      if (c.alpha == 1) {
        // Glass: average ink with fill (integer 50/50) so the backing shows through.
        const int r = (int((look.ink >> 16) & 0xFF) + int((look.fill >> 16) & 0xFF)) / 2;
        const int g = (int((look.ink >> 8) & 0xFF) + int((look.fill >> 8) & 0xFF)) / 2;
        const int b = (int(look.ink & 0xFF) + int(look.fill & 0xFF)) / 2;
        look.ink = rgb(r, g, b);
      }
      break;
    }
    default: {  // matrix green
      switch (c.glyph) {
        case '#': look.ink = 0xFF00BE46; break;
        case '|': look.ink = 0xFF78FFC8; break;
        case 'k':
        case 'E':
        case '*': look.ink = 0xFFD2FFD2; break;
        case 'o': look.ink = 0xFF006E32; break;
        default: {
          const char* p = std::strchr(kRamp, c.glyph);
          const int idx = p ? int(p - kRamp) : 0;
          look.ink = rgb(0, 60 + idx * 17, 20 + idx * 4);
          break;
        }
      }
      break;
    }
  }
  if (style != ViewStyle::Solid && style != ViewStyle::Depth && display.textureColor &&
      (texColor & 0x00FFFFFFu) != 0 && look.glyph != ' ' && (c.depth & 7) != kSkyBand) {
    const int shade = clampi((c.light & 15) * 17, 0, 255);
    look.ink = scaleRgb(texColor, shade);
  }
  applyDisplay(look, c, display, false);
  return look;
}

CellLook decodeCell(const AsciiFrame& f, size_t i, ViewStyle style, const DisplaySettings& display) {
  const uint32_t tex = (i < f.texColor.size()) ? f.texColor[i] : 0;
  CellLook look = decodeCell(cellAt(f, i), style, display, tex);
  if (display.edgeLift > 0 && neighbourEdge(f, i)) {
    const CellFields c = unpackCell(cellAt(f, i));
    if ((c.depth & 7) != kSkyBand) look.ink = lightenInk(look.ink, display.edgeLift);
  }
  return look;
}

}  // namespace litholattice::nemoview
