#include "litholattice/render/hud_ui_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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
// ---- Textured icons (Phase B, 2026-11-09): the icon atlas builder ----
// One icon draw = one tile + one textured quad (JD's law: one instance
// drawn per stack; the count stays a text badge). See the class comment
// in hud_ui_renderer.h for the exactness argument of the two classes.
namespace {
HudIconStats g_hudIconStats;
}  // namespace

cHudIconAtlas& hudIconAtlas() {
  static cHudIconAtlas a;
  return a;
}
const HudIconStats& hudIconStats() { return g_hudIconStats; }

bool cHudIconAtlas::growFor(int w, int h) {
  int nw = canvasW_, nh = canvasH_;
  if (w > nw && nw < kCap) nw = std::min(kCap, std::max(nw * 2, w));
  else if (h > nh && nh < kCap) nh = std::min(kCap, std::max(nh * 2, h));
  else if (nw <= nh && nw < kCap) nw = std::min(kCap, nw * 2);
  else if (nh < kCap) nh = std::min(kCap, nh * 2);
  else return false;
  std::vector<uint8_t> px(size_t(nw) * size_t(nh) * 4, 0);
  for (int y = 0; y < canvasH_; ++y)
    std::memcpy(&px[size_t(y) * size_t(nw) * 4], &canvasPx_[size_t(y) * size_t(canvasW_) * 4], size_t(canvasW_) * 4);
  canvasPx_ = std::move(px);
  canvasW_ = nw;
  canvasH_ = nh;
  ++grows_;
  return true;
}

bool cHudIconAtlas::place(int w, int h, int& x, int& y) {
  if (canvasW_ == 0) {
    canvasW_ = canvasH_ = 1024;
    canvasPx_.assign(size_t(1024) * 1024 * 4, 0);
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (curX_ + w <= canvasW_ && curY_ + h <= canvasH_) {
      x = curX_;
      y = curY_;
      curX_ += w;
      shelfH_ = std::max(shelfH_, h);
      return true;
    }
    if (curY_ + shelfH_ + h <= canvasH_) {  // next shelf down
      curY_ += shelfH_;
      curX_ = 0;
      shelfH_ = 0;
      continue;
    }
    if (!growFor(w, h)) return false;  // growth never moves a placed tile
  }
  return false;
}

int cHudIconAtlas::tileFor(const Image& src, bool cutout, int fitW, int fitH) {
  const int tw = cutout ? src.width() : fitW;
  const int th = cutout ? src.height() : fitH;
  if (tw <= 0 || th <= 0 || tw > kCap || th > kCap) {
    ++g_hudIconStats.declines;
    return -1;
  }
  for (size_t i = 0; i < tiles_.size(); ++i) {
    const Entry& e = tiles_[i];
    if (e.src == &src && e.gen == g_iconGen && e.cutout == cutout && e.w == tw && e.h == th) return int(i);
  }
  int x = 0, y = 0;
  if (!place(tw, th, x, y)) {
    ++g_hudIconStats.declines;
    return -1;
  }
  auto put = [&](int tx, int ty, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* o = &canvasPx_[(size_t(y + ty) * size_t(canvasW_) + size_t(x + tx)) * 4];
    o[0] = r;
    o[1] = g;
    o[2] = b;
    o[3] = a;
  };
  if (cutout) {
    // The raster magnified path draws exactly this: texels below the
    // 128 boundary absent, the rest fully opaque at the tint alpha.
    for (int ty = 0; ty < th; ++ty)
      for (int tx = 0; tx < tw; ++tx) {
        const uint8_t* p = src.pixel(tx, ty);
        if (p[3] >= 128) put(tx, ty, p[0], p[1], p[2], 255);
        else put(tx, ty, 0, 0, 0, 0);
      }
  } else {
    // The same fitted image the raster path paints (same cache, same
    // box filter), cropped to the fitted size the raster path reads.
    const Image& fit = (src.width() == fitW && src.height() == fitH) ? src : fittedIcon(src, fitW, fitH);
    const int fw = std::min(tw, fit.width()), fh = std::min(th, fit.height());
    for (int ty = 0; ty < fh; ++ty)
      for (int tx = 0; tx < fw; ++tx) {
        const uint8_t* p = fit.pixel(tx, ty);
        if (p[3] < 8) put(tx, ty, 0, 0, 0, 0);  // the raster path skips these; zeroed = contributes nothing
        else put(tx, ty, p[0], p[1], p[2], p[3]);
      }
  }
  tiles_.push_back({&src, g_iconGen, cutout, tw, th, x, y});
  dirty_ = true;
  return int(tiles_.size()) - 1;
}

void cHudIconAtlas::tileUv(int entry, float& u0, float& v0, float& u1, float& v1) const {
  const Entry& e = tiles_[size_t(entry)];
  u0 = float(e.x) / float(canvasW_);
  u1 = float(e.x + e.w) / float(canvasW_);
  v0 = float(e.y) / float(canvasH_);
  v1 = float(e.y + e.h) / float(canvasH_);
}

void cHudIconAtlas::publish() {
  if (!dirty_) return;
  snap_ = std::make_shared<Image>(canvasW_, canvasH_, canvasPx_);  // one copy, on rebuild frames only
  ++version_;
  dirty_ = false;
}

void cHudIconAtlas::retire() {
  tiles_.clear();
  canvasPx_.clear();
  canvasW_ = canvasH_ = 0;
  curX_ = curY_ = shelfH_ = 0;
  snap_.reset();
  ++version_;  // a retirement is itself a version move: nothing keys against a dead atlas
  dirty_ = false;
}

// 2026-11-08 (WS4, perf diagnosis item (d)): icon expansion cache and
// vertical run merging. HudDraw has no texture/UV channel (the HUD
// vertex path samples only the font atlas; see the header note: "the
// HUD has no texture path yet"), so a fitted icon still expands to
// coloured rects... but the expansion of a given icon at a given size
// is now computed ONCE and replayed (translated) on later paints, and
// within one expansion, runs that repeat on the row below extend the
// same rect downward instead of emitting a new one. Pixel output is
// identical in both cases: same coverage, same colours, same per-draw
// clip; the rects of one icon never overlap, so extending one cannot
// paint over a neighbour.
struct IconDraws { const Image* src; int w, h; float alpha; float tr, tg, tb; uint64_t gen; std::vector<HudDraw> draws; };
std::vector<IconDraws>& iconDrawCache() { static std::vector<IconDraws> c; return c; }

void invalidateHudIconCache() {
  ++g_iconGen;
  iconFitCache().clear();
  iconDrawCache().clear();  // WS4: expanded-draw cache invalidates with the fits
  hudIconAtlas().retire();  // textured icons: tiles key on the same generation discipline
}

void cHudUiRenderer::drawImage(const lithogui::Rect& r, int textureId, lithogui::Color tint) {
  const float imageAlpha = tint.a;
  const Image* icon0 = icons_ ? icons_(textureId) : nullptr;
  if (icon0 && !icon0->empty()) {
    const float x0 = snapPx(r.x), y0 = snapPx(r.y);
    const int pw = int(snapPx(r.x + r.w) - x0), ph = int(snapPx(r.y + r.h) - y0);
    if (pw > 0 && ph > 0) {
      // Textured lane (Phase B): one quad per icon, sampling the
      // layer's atlas snapshot. The class test is the raster path's
      // own (fitted when the source covers the rect, cutout when it
      // magnifies). A decline is counted and the raster lane below
      // runs exactly as before... never a silent partial migration.
      const bool fitted = icon0->width() >= pw && icon0->height() >= ph;
      const int entry = hudIconAtlas().tileFor(*icon0, !fitted, fitted ? pw : 0, fitted ? ph : 0);
      if (entry >= 0) {
        HudDraw d;
        d.x = x0;
        d.y = y0;
        d.w = fitted ? float(pw) : snapPx(r.x + r.w) - x0;
        d.h = fitted ? float(ph) : snapPx(r.y + r.h) - y0;
        d.r = tint.r;
        d.g = tint.g;
        d.b = tint.b;
        d.a = tint.a;
        d.icon = true;
        d.clip = clip();
        iconFixups_.push_back({layer_.draws.size(), entry});
        layer_.draws.push_back(std::move(d));
        ++iconEmitted_;
        ++g_hudIconStats.textured;
        return;
      }
      ++g_hudIconStats.rasterFallback;
    }
    if (pw > 0 && ph > 0 && icon0->width() >= pw && icon0->height() >= ph) {
      const Image& fit = (icon0->width() == pw && icon0->height() == ph) ? *icon0 : fittedIcon(*icon0, pw, ph);
      const int fw = std::min(pw, fit.width()), fh = std::min(ph, fit.height());
      auto& cache = iconDrawCache();
      for (const IconDraws& e : cache)
        if (e.src == icon0 && e.w == fw && e.h == fh && e.alpha == imageAlpha && e.tr == tint.r && e.tg == tint.g &&
            e.tb == tint.b && e.gen == g_iconGen) {
          const HudClip cl = clip();
          for (HudDraw d : e.draws) {
            d.x += x0;
            d.y += y0;
            d.clip = cl;
            layer_.draws.push_back(std::move(d));
          }
          return;
        }
      const size_t firstDraw = layer_.draws.size();
      // Open vertical spans from the previous row: a run identical in
      // x, width and colour extends its rect instead of emitting anew.
      struct Span { size_t draw; float x, w, bottom, r, g, b, a; };
      std::vector<Span> prev, cur;
      auto emitCell = [&](float ex, float ey, float ew, const lithogui::Color& col) {
        const float sx = snapPx(ex), sy = snapPx(ey);
        const float sw = snapPx(ex + ew) - sx, sh = snapPx(ey + 1.f) - sy;
        if (sw <= 0 || sh <= 0) return;
        for (const Span& s : prev)
          if (s.x == sx && s.w == sw && s.bottom == sy && s.r == col.r && s.g == col.g && s.b == col.b && s.a == col.a) {
            HudDraw& d = layer_.draws[s.draw];
            d.h = (sy + sh) - d.y;
            cur.push_back({s.draw, sx, sw, sy + sh, col.r, col.g, col.b, col.a});
            return;
          }
        HudDraw d;
        d.x = sx;
        d.y = sy;
        d.w = sw;
        d.h = sh;
        d.r = col.r;
        d.g = col.g;
        d.b = col.b;
        d.a = col.a;
        d.clip = clip();
        layer_.draws.push_back(std::move(d));
        cur.push_back({layer_.draws.size() - 1, sx, sw, sy + sh, col.r, col.g, col.b, col.a});
      };
      for (int y = 0; y < fh; ++y) {
        cur.clear();
        for (int x = 0; x < fw;) {
          const uint8_t* p = fit.pixel(x, y);
          if (p[3] < 8) { ++x; continue; }
          int run = 1;
          while (x + run < fw) {
            const uint8_t* q = fit.pixel(x + run, y);
            if (q[0] != p[0] || q[1] != p[1] || q[2] != p[2] || q[3] != p[3]) break;
            ++run;
          }
          emitCell(x0 + float(x), y0 + float(y), float(run),
                   {p[0] / 255.f * tint.r, p[1] / 255.f * tint.g, p[2] / 255.f * tint.b, p[3] / 255.f * tint.a});
          x += run;
        }
        prev.swap(cur);
      }
      if (cache.size() > 512) cache.clear();
      IconDraws entry{icon0, fw, fh, imageAlpha, tint.r, tint.g, tint.b, g_iconGen, {}};
      entry.draws.reserve(layer_.draws.size() - firstDraw);
      for (size_t i = firstDraw; i < layer_.draws.size(); ++i) {
        HudDraw d = layer_.draws[i];
        d.x -= x0;
        d.y -= y0;
        entry.draws.push_back(std::move(d));
      }
      cache.push_back(std::move(entry));
      return;
    }
  }
  // JD's catch rule (2026-11-10, GUI draw-cost program cure 4): the
  // magnified lane below reaches here only when the atlas DECLINED
  // above (the fitted lane's textured attempt runs first and returns);
  // a magnified draw whose tile was declined re-rasterizes per cell on
  // every repaint... so the catch gives this lane the same treatment:
  // capture the rasterization ONCE into an atlas tile keyed by (source
  // identity, fitted size, tint), then sample the tile forever. The
  // rasterization is rendered offscreen at the tile's own resolution
  // (the source's native size: the magnified draw is a NEAREST
  // magnification of the source, so a tile holding the thresholded
  // source reproduces it exactly... which is precisely the cutout
  // class; the catch and the class are the same mechanism, and this
  // branch exists for sources whose fitted-lane attempt declined for
  // size reasons on the FITTED side while the native size still fits).
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
  iconFixups_.clear();
  iconEmitted_ = 0;
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
      case lithogui::DrawCmd::Kind::ImageQuad: {
        lithogui::Color t = cmd.imageTint;
        t.a = cmd.imageAlpha;  // alpha rides the command's clamped field; RGB rides imageTint
        drawImage(cmd.rect, cmd.textureId, t);
        break;
      }
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
  if (iconEmitted_ > 0) {
    // Emitter census (textured icons step 3): every icon draw this
    // render() emitted was counted at its lane decision in drawImage
    // (hudIconStats(): textured / rasterFallback / declines)... the
    // census is session-global and read by the selftest and the
    // program report; a surface that silently stayed raster shows up
    // as fallback growth, never as an invisible partial migration.
    // Textured icons: settle the atlas for this layer. Publishing is
    // copy-on-grow (one snapshot copy per rebuild that added tiles);
    // UVs are read back from the entries now, against the canvas as
    // it stands, so a layer rendered earlier keeps sampling the same
    // pixels through its own snapshot (tiles never move).
    cHudIconAtlas& atlas = hudIconAtlas();
    if (atlas.dirty()) atlas.publish();
    if (atlas.snapshot() && atlas.version() >= layer_.iconAtlasVersion) {
      layer_.iconAtlas = atlas.snapshot();
      layer_.iconAtlasVersion = atlas.version();
    }
    for (const auto& fx : iconFixups_)
      atlas.tileUv(fx.second, layer_.draws[fx.first].iu0, layer_.draws[fx.first].iv0, layer_.draws[fx.first].iu1,
                   layer_.draws[fx.first].iv1);
    iconFixups_.clear();
    iconEmitted_ = 0;
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
