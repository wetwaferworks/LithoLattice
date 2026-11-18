// ASCII renderer pipeline — implementation (EXPERIMENT TIER, JD
// 2026-10-13). See software_ascii.h for the design and
// Ascii-Renderer-2026-10-13.md for the survey + economics.
#include "litholattice/render/software_ascii.h"

#include "font_data.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "litholattice/systerian/isysterian.h"

namespace litholattice {

const char* asciiGlyphSetName(AsciiGlyphSet set) {
  return set == AsciiGlyphSet::cSysterian ? "systerian" : "ascii";
}

bool asciiGlyphSetFromName(const char* name, AsciiGlyphSet& out) {
  if (!name) return false;
  const std::string_view v(name);
  if (v == "ascii") { out = AsciiGlyphSet::Ascii; return true; }
  if (v == "systerian") { out = AsciiGlyphSet::cSysterian; return true; }
  return false;
}

namespace {

// 5x7 bitmap font for the ten ramp glyphs, rows top-to-bottom, '#' = lit.
// Hand-drawn for this module; covers exactly asciiRamp()'s characters.
const char* glyphRows(char c, int row) {
  static const char* kSpace[7] = {".....", ".....", ".....", ".....",
                                  ".....", ".....", "....."};
  static const char* kDot[7] = {".....", ".....", ".....", ".....",
                                ".....", ".##..", ".##.."};
  static const char* kColon[7] = {".....", ".##..", ".##..", ".....",
                                  ".....", ".##..", ".##.."};
  static const char* kDash[7] = {".....", ".....", ".....", ".####",
                                 ".....", ".....", "....."};
  static const char* kEq[7] = {".....", ".....", ".####", ".....",
                               ".####", ".....", "....."};
  static const char* kPlus[7] = {".....", "..#..", "..#..", ".###.",
                                 "..#..", "..#..", "....."};
  static const char* kStar[7] = {".....", ".#.#.", "..#..", ".###.",
                                 "..#..", ".#.#.", "....."};
  static const char* kHash[7] = {".#.#.", ".#.#.", "#####", ".#.#.",
                                 "#####", ".#.#.", ".#.#."};
  static const char* kPct[7] = {"##..#", "##.#.", "...#.", "..#..",
                                ".#...", ".#.##", "#..##"};
  static const char* kAt[7] = {".###.", "#...#", "#.###", "#.#.#",
                               "#....", "#....", ".###."};
  switch (c) {
    case '.': return kDot[row];
    case ':': return kColon[row];
    case '-': return kDash[row];
    case '=': return kEq[row];
    case '+': return kPlus[row];
    case '*': return kStar[row];
    case '#': return kHash[row];
    case '%': return kPct[row];
    case '@': return kAt[row];
    default: return kSpace[row];
  }
}

struct CellAvg {
  float r = 0, g = 0, b = 0, luma = 0;
  char glyph = ' ';
  int level = 0;  ///< full-range value level in [0, span] (JD amendment 2026-10-13)
};

// Grid geometry shared by the text and image forms: cells tile the frame
// with a ~1:2 (w:h) character aspect. Returns false for degenerate input.
bool gridGeometry(const SoftwareFramebuffer& fb, int cols, int& outCols,
                  int& outRows, float& cellW, float& cellH) {
  if (fb.width < 8 || fb.height < 8) return false;
  outCols = std::clamp(cols, 8, int(fb.width));
  cellW = float(fb.width) / float(outCols);
  cellH = cellW * 2.f;
  outRows = std::max(1, int(std::lround(float(fb.height) / cellH)));
  cellH = float(fb.height) / float(outRows);  // exact tiling
  return true;
}

std::vector<CellAvg> summarize(const SoftwareFramebuffer& fb, int cols, int span,
                               int& outCols, int& outRows) {
  float cellW, cellH;
  if (!gridGeometry(fb, cols, outCols, outRows, cellW, cellH)) return {};
  const char* ramp = asciiRamp();
  std::vector<CellAvg> cells(size_t(outCols) * size_t(outRows));
  for (int cy = 0; cy < outRows; ++cy) {
    const int y0 = int(cy * cellH), y1 = std::max(y0 + 1, int((cy + 1) * cellH));
    for (int cx = 0; cx < outCols; ++cx) {
      const int x0 = int(cx * cellW), x1 = std::max(x0 + 1, int((cx + 1) * cellW));
      double r = 0, g = 0, b = 0;
      int n = 0;
      for (int y = y0; y < y1 && y < int(fb.height); ++y)
        for (int x = x0; x < x1 && x < int(fb.width); ++x) {
          const uint32_t p = fb.pixels[size_t(y) * fb.width + size_t(x)];
          r += (p >> 16) & 0xFF;
          g += (p >> 8) & 0xFF;
          b += p & 0xFF;
          ++n;
        }
      CellAvg c;
      if (n > 0) {
        c.r = float(r / n);
        c.g = float(g / n);
        c.b = float(b / n);
      }
      c.luma = (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) * (1.f / 255.f);
      // JD-RULED AMENDMENT (2026-10-13): level at FULL span resolution
      // (default span 255 = 256 steps) — a deliberate exception to the
      // game's 64-scale conventions. The level selects the ramp glyph
      // AND is the cSysterian glyph value; the tint beside it is the
      // cell's average framebuffer colour (the rendered, SVG-textured
      // variance), never a flat per-block colour.
      c.level = asciiValueLevel(c.luma, span);
      c.glyph = ramp[asciiRampIndex(c.level, span)];
      cells[size_t(cy) * size_t(outCols) + size_t(cx)] = c;
    }
  }
  return cells;
}

}  // namespace

std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGridInfo* info) {
  return framebufferToAsciiText(fb, cols, AsciiGlyphSet::Ascii, info);
}

std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGlyphSet set, AsciiGridInfo* info) {
  return framebufferToAsciiText(fb, cols, set, info, kAsciiDefaultValueSpan);
}

std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGlyphSet set, AsciiGridInfo* info, int span) {
  int gc = 0, gr = 0;
  const auto cells = summarize(fb, cols, span, gc, gr);
  std::string out;
  if (cells.empty()) return out;
  if (set == AsciiGlyphSet::cSysterian) {
    // Glyph-value grid (JD amendment 2026-10-13): each cell is its
    // full-range LEVEL (0..span, default 0..255) — the value fed to
    // composedGlyph, i.e. the script's native encoding — as three
    // zero-padded fixed digits, exact and parseable. (The experiment
    // build emitted the ramp characters' code points, 32..64, two
    // digits; JD's ruling supersedes that subset.)
    out.reserve(size_t(gc * 3 + 1) * size_t(gr));
    for (int y = 0; y < gr; ++y) {
      for (int x = 0; x < gc; ++x) {
        const unsigned v = unsigned(
            cells[size_t(y) * size_t(gc) + size_t(x)].level);
        out += char('0' + (v / 100) % 10);
        out += char('0' + (v / 10) % 10);
        out += char('0' + v % 10);
      }
      out += '\n';
    }
  } else {
    out.reserve(size_t(gc + 1) * size_t(gr));
    for (int y = 0; y < gr; ++y) {
      for (int x = 0; x < gc; ++x) out += cells[size_t(y) * size_t(gc) + size_t(x)].glyph;
      out += '\n';
    }
  }
  if (info) {
    info->cols = gc;
    info->rows = gr;
    info->textBytes = out.size();
  }
  return out;
}

void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols,
                             std::string* textOut, AsciiGridInfo* info) {
  framebufferToAsciiImage(fb, cols, AsciiGlyphSet::Ascii, textOut, info);
}

void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols, AsciiGlyphSet set,
                             std::string* textOut, AsciiGridInfo* info) {
  framebufferToAsciiImage(fb, cols, set, textOut, info, kAsciiDefaultValueSpan);
}

void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols, AsciiGlyphSet set,
                             std::string* textOut, AsciiGridInfo* info, int span) {
  int gc = 0, gr = 0;
  const auto cells = summarize(fb, cols, span, gc, gr);
  if (cells.empty()) return;
  if (textOut) {
    *textOut = framebufferToAsciiText(fb, cols, set, info, span);
  } else if (info) {
    info->cols = gc;
    info->rows = gr;
    info->textBytes = (set == AsciiGlyphSet::cSysterian ? size_t(gc * 3 + 1) : size_t(gc + 1)) *
                       size_t(gr);
  }
  if (set == AsciiGlyphSet::cSysterian) {
    // Draw the real glyphs: the codec's generative stroke geometry
    // (100x140 grid) scaled into each cell, tinted by the cell's average
    // colour; rivet dots at every graph vertex (JD's rivet rule).
    const float cellW = float(fb.width) / float(gc);
    const float cellH = float(fb.height) / float(gr);
    std::vector<uint32_t> img(size_t(fb.width) * fb.height, 0xFF000000u);
    auto plot = [&](int x, int y, uint32_t tint) {
      if (x >= 0 && y >= 0 && x < int(fb.width) && y < int(fb.height))
        img[size_t(y) * fb.width + size_t(x)] = tint;
    };
    for (int cy = 0; cy < gr; ++cy) {
      for (int cx = 0; cx < gc; ++cx) {
        const CellAvg& c = cells[size_t(cy) * size_t(gc) + size_t(cx)];
        const uint32_t tint = 0xFF000000u |
                              (uint32_t(std::clamp(c.r, 0.f, 255.f)) << 16) |
                              (uint32_t(std::clamp(c.g, 0.f, 255.f)) << 8) |
                              uint32_t(std::clamp(c.b, 0.f, 255.f));
        // JD amendment (2026-10-13): the glyph IS the cell's full-range
        // level (0..span, default 0..255), composed directly by the
        // codec — not the ramp character's code point (32..64 subset).
        const systerian::SysterianGlyph g =
            systerian::composedGlyph(uint32_t(c.level));
        const float ox = cx * cellW, oy = cy * cellH;
        const float sx = cellW / float(systerian::kGridWidth);
        const float sy = cellH / float(systerian::kGridHeight);
        const int thick = std::max(1, int(std::lround(std::min(sx, sy) * 3.0)));
        for (const auto& seg : g.segments) {
          const float ax = ox + float(seg.a.x) * sx, ay = oy + float(seg.a.y) * sy;
          const float bx = ox + float(seg.b.x) * sx, by = oy + float(seg.b.y) * sy;
          const int steps = std::max(1, int(std::ceil(
              std::max(std::fabs(bx - ax), std::fabs(by - ay)))));
          for (int s = 0; s <= steps; ++s) {
            const int px = int(std::lround(ax + (bx - ax) * float(s) / float(steps)));
            const int py = int(std::lround(ay + (by - ay) * float(s) / float(steps)));
            for (int dy = 0; dy < thick; ++dy)
              for (int dx = 0; dx < thick; ++dx) plot(px + dx, py + dy, tint);
          }
        }
        const int rr = std::max(1, int(std::lround(5.4 * std::min(sx, sy))));
        for (const auto& p : g.rivets) {
          const int px = int(std::lround(ox + float(p.x) * sx));
          const int py = int(std::lround(oy + float(p.y) * sy));
          for (int dy = -rr; dy <= rr; ++dy)
            for (int dx = -rr; dx <= rr; ++dx)
              if (dx * dx + dy * dy <= rr * rr) plot(px + dx, py + dy, tint);
        }
      }
    }
    fb.pixels = std::move(img);
    return;
  }
  const float cellW = float(fb.width) / float(gc);
  const float cellH = float(fb.height) / float(gr);
  std::vector<uint32_t> img(size_t(fb.width) * fb.height, 0xFF000000u);
  for (int cy = 0; cy < gr; ++cy) {
    for (int cx = 0; cx < gc; ++cx) {
      const CellAvg& c = cells[size_t(cy) * size_t(gc) + size_t(cx)];
      if (c.glyph == ' ') continue;
      const uint32_t tint = 0xFF000000u | (uint32_t(std::clamp(c.r, 0.f, 255.f)) << 16) |
                            (uint32_t(std::clamp(c.g, 0.f, 255.f)) << 8) |
                            uint32_t(std::clamp(c.b, 0.f, 255.f));
      const int x0 = int(cx * cellW), x1 = int((cx + 1) * cellW);
      const int y0 = int(cy * cellH), y1 = int((cy + 1) * cellH);
      for (int y = y0; y < y1 && y < int(fb.height); ++y) {
        const int grow = std::min(6, int(float(y - y0) * 7.f / std::max(1.f, cellH)));
        const char* bits = glyphRows(c.glyph, grow);
        for (int x = x0; x < x1 && x < int(fb.width); ++x) {
          const int gcol = std::min(4, int(float(x - x0) * 5.f / std::max(1.f, cellW)));
          if (bits[gcol] == '#') img[size_t(y) * fb.width + size_t(x)] = tint;
        }
      }
    }
  }
  fb.pixels = std::move(img);
}


bool asciiGridGeometry(int fbW, int fbH, int cols, AsciiGridGeom& out) {
  if (fbW < 8 || fbH < 8) return false;
  out.cols = std::clamp(cols, 8, fbW);
  out.cellW = float(fbW) / float(out.cols);
  out.cellH = out.cellW * 2.f;
  out.rows = std::max(1, int(std::lround(float(fbH) / out.cellH)));
  out.cellH = float(fbH) / float(out.rows);
  return out.cols > 0 && out.rows > 0 && out.cellW > 0.f && out.cellH > 0.f;
}

void asciiPointerToCell(const AsciiGridGeom& g, float px, float py, int& cx, int& cy) {
  cx = g.cols <= 1 ? 0 : std::clamp(int(px / g.cellW), 0, g.cols - 1);
  cy = g.rows <= 1 ? 0 : std::clamp(int(py / g.cellH), 0, g.rows - 1);
}

void asciiCellCenter(const AsciiGridGeom& g, int cx, int cy, float& px, float& py) {
  px = (float(cx) + 0.5f) * g.cellW;
  py = (float(cy) + 0.5f) * g.cellH;
}

void asciiSnapPointer(const AsciiGridGeom& g, float& px, float& py) {
  int cx = 0, cy = 0;
  asciiPointerToCell(g, px, py, cx, cy);
  asciiCellCenter(g, cx, cy, px, py);
}

namespace {

void cellPixelRect(const AsciiGridGeom& g, int cx, int cy, int fbW, int fbH, int& x0, int& y0, int& x1, int& y1) {
  x0 = int(float(cx) * g.cellW);
  y0 = int(float(cy) * g.cellH);
  x1 = int(float(cx + 1) * g.cellW);
  y1 = int(float(cy + 1) * g.cellH);
  if (x1 <= x0) x1 = x0 + 1;
  if (y1 <= y0) y1 = y0 + 1;
  x0 = std::clamp(x0, 0, fbW);
  y0 = std::clamp(y0, 0, fbH);
  x1 = std::clamp(x1, 0, fbW);
  y1 = std::clamp(y1, 0, fbH);
}

uint32_t packColor(float r, float g, float b, float a) {
  auto ch = [](float v) {
    return uint32_t(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
  };
  return (ch(a) << 24) | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

uint32_t blendOver(uint32_t dst, uint32_t src) {
  const int sa = int((src >> 24) & 0xFF);
  if (sa >= 250) return 0xFF000000u | (src & 0x00FFFFFFu);
  if (sa <= 0) return dst;
  const int da = 255 - sa;
  auto mix = [&](int s, int d) { return (s * sa + d * da + 127) / 255; };
  const int r = mix(int((src >> 16) & 0xFF), int((dst >> 16) & 0xFF));
  const int g = mix(int((src >> 8) & 0xFF), int((dst >> 8) & 0xFF));
  const int b = mix(int(src & 0xFF), int(dst & 0xFF));
  return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

void fillCell(SoftwareFramebuffer& fb, const AsciiGridGeom& g, int cx, int cy, uint32_t rgba) {
  if (cx < 0 || cy < 0 || cx >= g.cols || cy >= g.rows) return;
  int x0, y0, x1, y1;
  cellPixelRect(g, cx, cy, int(fb.width), int(fb.height), x0, y0, x1, y1);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      uint32_t& p = fb.pixels[size_t(y) * fb.width + size_t(x)];
      p = blendOver(p, rgba);
    }
}

// Mid-line box strokes in a cell: bits 1=N, 2=E, 4=S, 8=W.
void strokeCell(SoftwareFramebuffer& fb, const AsciiGridGeom& g, int cx, int cy, uint32_t rgba, int arms) {
  if (cx < 0 || cy < 0 || cx >= g.cols || cy >= g.rows || arms == 0) return;
  int x0, y0, x1, y1;
  cellPixelRect(g, cx, cy, int(fb.width), int(fb.height), x0, y0, x1, y1);
  if (x1 - x0 < 2 || y1 - y0 < 2) {
    fillCell(fb, g, cx, cy, rgba);
    return;
  }
  const int t = std::max(1, std::min((x1 - x0), (y1 - y0)) / 8);
  const int mx = (x0 + x1) / 2;
  const int my = (y0 + y1) / 2;
  auto stamp = [&](int x, int y) {
    if (x >= x0 && x < x1 && y >= y0 && y < y1)
      fb.pixels[size_t(y) * fb.width + size_t(x)] = blendOver(fb.pixels[size_t(y) * fb.width + size_t(x)], rgba);
  };
  auto barH = [&](int xa, int xb) {
    if (xb < xa) std::swap(xa, xb);
    for (int y = my - t / 2; y < my - t / 2 + t; ++y)
      for (int x = xa; x < xb; ++x) stamp(x, y);
  };
  auto barV = [&](int ya, int yb) {
    if (yb < ya) std::swap(ya, yb);
    for (int y = ya; y < yb; ++y)
      for (int x = mx - t / 2; x < mx - t / 2 + t; ++x) stamp(x, y);
  };
  if (arms & 1) barV(y0, my + t);          // N
  if (arms & 4) barV(my, y1);              // S
  if (arms & 8) barH(x0, mx + t);          // W
  if (arms & 2) barH(mx, x1);              // E
}

void glyphCell(SoftwareFramebuffer& fb, const AsciiGridGeom& g, int cx, int cy, char ch, uint32_t rgba) {
  if (cx < 0 || cy < 0 || cx >= g.cols || cy >= g.rows) return;
  const int ci = int(static_cast<unsigned char>(ch)) - font::kFirst;
  if (ci < 0 || ci >= font::kCount) return;
  const char* bits = font::kGlyphs[ci];
  int x0, y0, x1, y1;
  cellPixelRect(g, cx, cy, int(fb.width), int(fb.height), x0, y0, x1, y1);
  const int cw = std::max(1, x1 - x0), chh = std::max(1, y1 - y0);
  for (int y = y0; y < y1; ++y) {
    const int gy = std::min(font::kCellH - 1, (y - y0) * font::kCellH / chh);
    for (int x = x0; x < x1; ++x) {
      const int gx = std::min(font::kCellW - 1, (x - x0) * font::kCellW / cw);
      const char d = bits[gy * font::kCellW + gx];
      const int cov = d >= 'a' ? d - 'a' + 10 : d >= 'A' ? d - 'A' + 10 : d - '0';
      if (cov <= 0) continue;
      const int sa = int((rgba >> 24) & 0xFF) * cov / 15;
      const uint32_t src = (uint32_t(sa) << 24) | (rgba & 0x00FFFFFFu);
      uint32_t& p = fb.pixels[size_t(y) * fb.width + size_t(x)];
      p = blendOver(p, src);
    }
  }
}

bool clipContains(const HudClip& c, float x, float y) { return x >= c.x0 && y >= c.y0 && x < c.x1 && y < c.y1; }

void fillRectCells(SoftwareFramebuffer& fb, const AsciiGridGeom& g, float x, float y, float w, float h,
                   uint32_t rgba, const HudClip& clip, bool box) {
  if (w <= 0.f || h <= 0.f || ((rgba >> 24) & 0xFF) == 0) return;
  const bool thinH = h < g.cellH * 0.6f;
  const bool thinW = w < g.cellW * 0.6f;
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
  if (thinH && !thinW) {
    asciiPointerToCell(g, x, y + h * 0.5f, cx0, cy0);
    asciiPointerToCell(g, x + w - 0.01f, y + h * 0.5f, cx1, cy1);
    cy1 = cy0;
  } else if (thinW && !thinH) {
    asciiPointerToCell(g, x + w * 0.5f, y, cx0, cy0);
    asciiPointerToCell(g, x + w * 0.5f, y + h - 0.01f, cx1, cy1);
    cx1 = cx0;
  } else {
    // Cells whose centres sit inside the rect — matches mouse snap.
    asciiPointerToCell(g, x + g.cellW * 0.5f, y + g.cellH * 0.5f, cx0, cy0);
    asciiPointerToCell(g, x + std::max(0.f, w - g.cellW * 0.5f), y + std::max(0.f, h - g.cellH * 0.5f), cx1, cy1);
    // A rect smaller than a cell still paints the cell it covers.
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
  }
  if (cx1 < cx0) std::swap(cx0, cx1);
  if (cy1 < cy0) std::swap(cy0, cy1);
  for (int cy = cy0; cy <= cy1; ++cy)
    for (int cx = cx0; cx <= cx1; ++cx) {
      float px, py;
      asciiCellCenter(g, cx, cy, px, py);
      if (!clipContains(clip, px, py)) continue;
      if (thinH && !thinW) strokeCell(fb, g, cx, cy, rgba, 8 | 2);
      else if (thinW && !thinH) strokeCell(fb, g, cx, cy, rgba, 1 | 4);
      else fillCell(fb, g, cx, cy, rgba);
    }
  if (box && !thinH && !thinW && (cx1 > cx0 || cy1 > cy0)) {
    for (int cx = cx0; cx <= cx1; ++cx)
      for (int cy = cy0; cy <= cy1; ++cy) {
        float px, py;
        asciiCellCenter(g, cx, cy, px, py);
        if (!clipContains(clip, px, py)) continue;
        int arms = 0;
        if (cy == cy0) arms |= 1;
        if (cy == cy1) arms |= 4;
        if (cx == cx0) arms |= 8;
        if (cx == cx1) arms |= 2;
        if (arms) strokeCell(fb, g, cx, cy, rgba | 0xFF000000u, arms);
      }
  }
}

void textCells(SoftwareFramebuffer& fb, const AsciiGridGeom& g, float x, float y, std::string_view text,
               uint32_t rgba, float scale, const HudClip& clip) {
  if (text.empty() || ((rgba >> 24) & 0xFF) == 0) return;
  const float gw = float(kHudGlyphW) * std::max(1.f, scale);
  const float gh = float(kHudGlyphH) * std::max(1.f, scale);
  int cx = 0, cy = 0;
  asciiPointerToCell(g, x + gw * 0.5f, y + gh * 0.5f, cx, cy);
  for (size_t i = 0; i < text.size(); ++i) {
    const int ccx = cx + int(i);
    if (ccx >= g.cols) break;
    float px, py;
    asciiCellCenter(g, ccx, cy, px, py);
    if (!clipContains(clip, px, py)) continue;
    const char ch = text[i];
    if (ch == ' ') continue;
    glyphCell(fb, g, ccx, cy, ch, rgba);
  }
}

void paintLayer(SoftwareFramebuffer& fb, const AsciiGridGeom& g, const HudLayer& layer) {
  for (const HudRect& r : layer.rects)
    fillRectCells(fb, g, r.x, r.y, r.w, r.h, packColor(r.r, r.g, r.b, r.a), HudClip{}, false);
  for (const HudText& t : layer.texts)
    textCells(fb, g, t.x, t.y, t.text, packColor(t.r, t.g, t.b, t.a), layer.scale, HudClip{});
  for (const HudDraw& d : layer.draws) {
    const uint32_t rgba = packColor(d.r, d.g, d.b, d.a);
    if (d.icon || d.text.empty()) {
      // Icons become a tinted cell fill (and a '#' if the cell is large enough
      // to hold a glyph). Wide panels also get a box frame.
      const bool panel = !d.icon && d.w >= g.cellW * 2.f && d.h >= g.cellH * 2.f && d.a >= 0.4f;
      fillRectCells(fb, g, d.x, d.y, d.w, d.h, rgba, d.clip, panel);
      if (d.icon) {
        int cx = 0, cy = 0;
        asciiPointerToCell(g, d.x + d.w * 0.5f, d.y + d.h * 0.5f, cx, cy);
        float px, py;
        asciiCellCenter(g, cx, cy, px, py);
        if (clipContains(d.clip, px, py)) glyphCell(fb, g, cx, cy, '#', rgba | 0xFF000000u);
      }
    } else {
      textCells(fb, g, d.x, d.y, d.text, rgba, d.scale > 0.f ? d.scale : layer.scale, d.clip);
    }
  }
}

}  // namespace

void paintAsciiHud(SoftwareFramebuffer& fb, const FrameParams& p, int cols) {
  AsciiGridGeom g;
  if (!asciiGridGeometry(int(fb.width), int(fb.height), cols, g)) return;
  if (p.hudStatic.layer || p.hudDynamic.layer) {
    if (p.hudStatic.layer) paintLayer(fb, g, *p.hudStatic.layer);
    if (p.hudDynamic.layer) paintLayer(fb, g, *p.hudDynamic.layer);
  } else {
    paintLayer(fb, g, p.hud);
  }
  if (p.crosshair) {
    int cx = 0, cy = 0;
    asciiPointerToCell(g, float(fb.width) * 0.5f, float(fb.height) * 0.5f, cx, cy);
    glyphCell(fb, g, cx, cy, '+', 0xFFFFFFFFu);
  }
}


}  // namespace litholattice
