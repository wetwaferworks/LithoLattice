#include "litholattice/render/hud_ui_renderer.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace litholattice {

// Cortana 2026-10-16 [ENGINE] (JD crisp GUI): every UI rect and glyph run lands on whole pixels.
// Edges are rounded independently (not x and w), so adjacent rects never gap or overlap.
static float snapPx(float v) { return std::floor(v + 0.5f); }

// The old ui toolkit's text-scale snap, kept for the HUD's baked 8x16 glyph cell:
// scales below 1 stay 1, anything above rounds to a whole multiplier, so HUD text
// never lands on a fractional glyph cell.
static float snapTextScale(float s) { return s < 1.f ? 1.f : float(int(s + 0.5f)); }

void cHudUiRenderer::fillRect(const lithogui::Rect& r, lithogui::Color c) {
  HudDraw d;
  d.x = snapPx(r.x);
  d.y = snapPx(r.y);
  d.w = snapPx(r.x + r.w) - d.x;
  d.h = snapPx(r.y + r.h) - d.y;
  if (d.w <= 0 || d.h <= 0) return;
  d.r = c.r;
  d.g = c.g;
  d.b = c.b;
  d.a = c.a;
  d.clip = clip();
  layer_.draws.push_back(std::move(d));
}

void cHudUiRenderer::drawText(lithogui::Vec2 pos, const std::string& text, lithogui::Color c, float scale) {
  HudDraw d;
  d.x = snapPx(pos.x);
  d.y = snapPx(pos.y);
  d.r = c.r;
  d.g = c.g;
  d.b = c.b;
  d.a = c.a;
  d.scale = snapTextScale(scale);
  d.text = text;
  d.clip = clip();
  layer_.draws.push_back(std::move(d));
}

void cHudUiRenderer::drawLine(lithogui::Vec2 a, lithogui::Vec2 b, lithogui::Color c, float thickness) {
  // HudDraws are axis-aligned rects and text only, so a segment becomes its
  // bounding rect at the requested thickness. Axis-aligned segments (the
  // Trace-border fallback polylines, widget rules) are exact; a diagonal
  // segment is approximated by its bounding box — honest HUD limitation.
  const float t = std::max(1.0f, thickness);
  lithogui::Rect r;
  r.x = std::min(a.x, b.x);
  r.y = std::min(a.y, b.y);
  r.w = std::max(std::abs(b.x - a.x), a.y == b.y ? 0.0f : t);
  r.h = std::max(std::abs(b.y - a.y), a.x == b.x ? 0.0f : t);
  if (a.y == b.y) r.h = t;
  if (a.x == b.x) r.w = t;
  fillRect(r, c);
}

// 2026-10-25 (JD, crisp hotbar icons): a source icon larger than its on-screen rect is
// box-filtered once to the rect's exact whole-pixel size (cached), then drawn 1 cell = 1
// screen pixel, run-length merged per row, with alpha kept (smooth edges, no 50% cut).
// Never magnifies a high-res source; small sources keep the old pixel-art cell path.
namespace {
struct IconFit { const Image* src; int w, h; uint64_t gen; Image img; };
std::vector<IconFit>& iconFitCache() { static std::vector<IconFit> c; return c; }
uint64_t g_iconGen = 1;
const Image& fittedIcon(const Image& src, int w, int h) {
  auto& c = iconFitCache();
  for (const IconFit& f : c) if (f.src == &src && f.w == w && f.h == h && f.gen == g_iconGen) return f.img;
  if (c.size() > 512) c.clear();
  Image sq = downsampleIcon(src, std::max(w, h));  // icons are square; slots are square
  c.push_back({&src, w, h, g_iconGen, std::move(sq)});
  return c.back().img;
}
}  // namespace
void invalidateHudIconCache() { ++g_iconGen; iconFitCache().clear(); }

void cHudUiRenderer::drawImage(const lithogui::Rect& r, int textureId, float imageAlpha) {
  const lithogui::Color tint{1.f, 1.f, 1.f, imageAlpha};
  const Image* icon0 = icons_ ? icons_(textureId) : nullptr;
  if (icon0 && !icon0->empty()) {
    const float x0 = snapPx(r.x), y0 = snapPx(r.y);
    const int pw = int(snapPx(r.x + r.w) - x0), ph = int(snapPx(r.y + r.h) - y0);
    if (pw > 0 && ph > 0 && icon0->width() >= pw && icon0->height() >= ph) {
      const Image& fit = (icon0->width() == pw && icon0->height() == ph) ? *icon0 : fittedIcon(*icon0, pw, ph);
      const int fw = std::min(pw, fit.width()), fh = std::min(ph, fit.height());
      for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw;) {
          const uint8_t* p = fit.pixel(x, y);
          if (p[3] < 8) { ++x; continue; }
          int run = 1;
          while (x + run < fw) {
            const uint8_t* q = fit.pixel(x + run, y);
            if (q[0] != p[0] || q[1] != p[1] || q[2] != p[2] || q[3] != p[3]) break;
            ++run;
          }
          fillRect({x0 + float(x), y0 + float(y), float(run), 1.f},
                   {p[0] / 255.f * tint.r, p[1] / 255.f * tint.g, p[2] / 255.f * tint.b, p[3] / 255.f * tint.a});
          x += run;
        }
      return;
    }
  }
  if (const Image* icon = icon0; icon && !icon->empty()) {
    const float cw = r.w / float(icon->width()), ch = r.h / float(icon->height());
    for (int y = 0; y < icon->height(); ++y)
      for (int x = 0; x < icon->width(); ++x) {
        const uint8_t* p = icon->pixel(x, y);
        if (p[3] < 128) continue;
        // edges from the icon grid, so cells tile exactly after fillRect's pixel snap
        fillRect({r.x + float(x) * cw, r.y + float(y) * ch, cw, ch},
                 {p[0] / 255.f * tint.r, p[1] / 255.f * tint.g, p[2] / 255.f * tint.b, tint.a});
      }
    return;
  }
  const std::optional<lithogui::Color> swatch = images_ ? images_(textureId) : std::nullopt;
  const lithogui::Color base = swatch.value_or(lithogui::Color{1.f, 0.f, 1.f, 1.f});  // magenta: unknown image
  fillRect(r, {base.r * tint.r, base.g * tint.g, base.b * tint.b, base.a * tint.a});
}

void cHudUiRenderer::pushClip(const lithogui::Rect& r) {
  const HudClip cur = clip();
  clips_.push_back({std::max(cur.x0, snapPx(r.x)), std::max(cur.y0, snapPx(r.y)),
                    std::min(cur.x1, snapPx(r.x + r.w)), std::min(cur.y1, snapPx(r.y + r.h))});
}

void cHudUiRenderer::popClip() {
  if (!clips_.empty()) clips_.pop_back();
}

void cHudUiRenderer::render(const lithogui::DrawList& dl) {
  clips_.clear();
  for (const lithogui::DrawCmd& cmd : dl.commands()) {
    switch (cmd.kind) {
      case lithogui::DrawCmd::Kind::PushClip:
        pushClip(cmd.rect);
        break;
      case lithogui::DrawCmd::Kind::PopClip:
        popClip();
        break;
      case lithogui::DrawCmd::Kind::FillRect:
        fillRect(cmd.rect, cmd.color);
        break;
      case lithogui::DrawCmd::Kind::Line:
        if (cmd.points.size() >= 2) drawLine(cmd.points[0], cmd.points[1], cmd.color, cmd.thickness);
        break;
      case lithogui::DrawCmd::Kind::Polyline:
        for (size_t i = 1; i < cmd.points.size(); ++i)
          drawLine(cmd.points[i - 1], cmd.points[i], cmd.color, cmd.thickness);
        break;
      case lithogui::DrawCmd::Kind::TextRun: {
        // The HUD glyph cell is 8x16 at scale 1 (see renderer.h); the run's
        // origin rect is the top-left of the text box, baseline offset inside.
        const float scale = cmd.fontSize > 0.0f ? cmd.fontSize / 16.0f : 1.0f;
        drawText({cmd.rect.x, cmd.rect.y}, cmd.text, cmd.color, scale);
        break;
      }
      case lithogui::DrawCmd::Kind::ImageQuad:
        drawImage(cmd.rect, cmd.textureId, cmd.imageAlpha);
        break;
      case lithogui::DrawCmd::Kind::GlyphQuad:
        // Skipped, honestly: the HUD has no glyph-atlas texture path. Glyph
        // expansion only produces GlyphQuads when a glyph source is bound to
        // the UiContext; the default engine path binds none, so text arrives
        // as TextRuns and nothing is lost here.
        break;
      case lithogui::DrawCmd::Kind::CustomDraw:
        // Skipped: CustomDraw tag 1 is the shader Trace border, and the GUI
        // emits a fallback Polyline for the same rect immediately before it
        // in the stream — that polyline is what the HUD draws. A GL/Vulkan
        // shader pass can pick the tag up later without changing this code's
        // output contract.
        break;
    }
  }
  clips_.clear();
}

Image downsampleIcon(const Image& src, int size) {
  if (src.empty() || size <= 0) return {};
  std::vector<uint8_t> out(size_t(size) * size_t(size) * 4);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const int x0 = x * src.width() / size, x1 = std::max(x0 + 1, (x + 1) * src.width() / size);
      const int y0 = y * src.height() / size, y1 = std::max(y0 + 1, (y + 1) * src.height() / size);
      double c[3] = {0, 0, 0}, a = 0;
      int n = 0;
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx, ++n) {
          const uint8_t* p = src.pixel(sx, sy);
          for (int k = 0; k < 3; ++k) c[k] += p[k] * double(p[3]);
          a += p[3];
        }
      uint8_t* o = &out[(size_t(y) * size_t(size) + size_t(x)) * 4];
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(a > 0 ? c[k] / a + 0.5 : 0);
      o[3] = uint8_t(n ? a / n + 0.5 : 0);
    }
  return Image(size, size, std::move(out));
}

}  // namespace litholattice
