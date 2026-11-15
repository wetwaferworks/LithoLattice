// cHudUiRenderer: consumes a Litho GUI draw list (lithogui::DrawList) and appends
// ordered, CPU-clipped HudDraws to a HudLayer, so both backends (gl33 and Vulkan)
// render the GUI through buildHudVertices with no backend-specific GUI code.
// Images resolve through callbacks: a texture id either maps to an icon image
// (drawn as ONE textured quad sampling the layer's icon atlas snapshot...
// textured icons, Phase B 2026-11-09; the per-pixel raster expansion is the
// retained fallback lane), or to a colour swatch.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/render/renderer.h"
#include "litholattice/ui/litho_gui.h"

namespace litholattice {

/// Textured icons (Phase B, 2026-11-09): the engine-owned HUD icon atlas
/// builder. Icon draws register a tile here instead of expanding to
/// per-pixel rect draws; the layer then carries ONE textured quad per
/// icon plus a shared_ptr to the atlas snapshot its UVs address.
///
/// Two tile classes, each exact under NEAREST sampling (the sampler
/// discipline on every backend):
///   blend  (fitted class): the tile pixels are the same fittedIcon
///          box-filter output the raster path paints, cropped to the
///          fitted size, with alpha < 8 zeroed (the raster path skips
///          those texels; a zeroed texel contributes exactly nothing).
///   cutout (magnified class): the source with alpha thresholded to
///          0/255 at the 128 boundary, reproducing the raster path's
///          hard cutout literally.
/// Tiles are append-only within a generation and NEVER move: growth
/// extends the canvas (old tiles keep their positions), and publishing
/// is copy-on-grow... a snapshot a live layer references is never
/// mutated. Registration keys mirror the fitted-icon caches exactly
/// (source pointer + the g_iconGen generation), so F9 reload semantics
/// are inherited: invalidateHudIconCache() retires the builder and the
/// next registration starts a fresh atlas, while in-flight layers keep
/// their own (correct) snapshot. All state lives on the frame thread;
/// no lock is taken or needed anywhere in this class.
class cHudIconAtlas {
 public:
  static constexpr int kCap = 4096;  ///< atlas dimension cap (design bound 3)

  /// Registers (or finds) the tile for one icon draw and returns its
  /// entry index, or -1 to DECLINE (tile larger than the cap, or the
  /// atlas full at the cap)... the caller then takes the raster lane.
  /// `fitW`/`fitH` are the fitted draw size in pixels (blend class);
  /// the cutout class tiles at the source's own size.
  int tileFor(const Image& src, bool cutout, int fitW, int fitH);
  /// Normalized UV rect of a registered entry, against the CURRENT
  /// canvas. Callers re-read this at publish time (growth between
  /// registration and publish changes the denominators, never the
  /// positions), which is why tileFor returns an entry, not UVs.
  void tileUv(int entry, float& u0, float& v0, float& u1, float& v1) const;
  /// Publishes the working canvas as a new snapshot (one copy, only
  /// when tiles were added since the last publish).
  void publish();
  bool dirty() const { return dirty_; }
  std::shared_ptr<const Image> snapshot() const { return snap_; }
  uint64_t version() const { return version_; }
  size_t tileCount() const { return tiles_.size(); }
  int growCount() const { return grows_; }
  /// F9 discipline: forget every tile; the next publish is a fresh
  /// atlas at a higher version. Old snapshots stay alive in the layers
  /// that reference them.
  void retire();

 private:
  struct Entry {
    const Image* src;
    uint64_t gen;
    bool cutout;
    int w, h;  ///< tile size in pixels
    int x, y;  ///< tile position in the canvas (never moves)
  };
  bool place(int w, int h, int& x, int& y);
  bool growFor(int w, int h);
  std::vector<Entry> tiles_;
  std::vector<uint8_t> canvasPx_;  ///< working canvas, RGBA row-major
  int canvasW_ = 0, canvasH_ = 0;
  int curX_ = 0, curY_ = 0, shelfH_ = 0;
  std::shared_ptr<const Image> snap_;
  uint64_t version_ = 0;
  int grows_ = 0;
  bool dirty_ = false;
};

/// The one atlas builder (frame thread, like the icon caches below).
cHudIconAtlas& hudIconAtlas();

/// Icon-lane census for the whole session: how many icon draws took
/// the textured lane, how many fell back to the raster lane, and how
/// many registrations the atlas declined. Counted, never silent... a
/// partial migration shows up in these numbers or it does not happen.
struct HudIconStats {
  uint64_t textured = 0;
  uint64_t rasterFallback = 0;
  uint64_t declines = 0;
};
const HudIconStats& hudIconStats();

class cHudUiRenderer final {
 public:
  /// Colour swatch for a Litho GUI texture id (ImageQuad fallback path).
  using ImageResolver = std::function<std::optional<lithogui::Color>(int textureId)>;

  /// Pixel-art icons: a small RGBA image drawn as one rect per opaque pixel
  /// (alpha >= 50%), so the HUD needs no textures. Keep icons small
  /// (16 x 16, see downsampleIcon).
  using IconResolver = std::function<const Image*(int textureId)>;

  /// Appends to `layer.draws` (the caller clears the layer each frame).
  explicit cHudUiRenderer(HudLayer& layer, ImageResolver images = {}, IconResolver icons = {})
      : layer_(layer), images_(std::move(images)), icons_(std::move(icons)) {}

  /// Translates one frame of Litho GUI draw commands into HudDraws, in order.
  void render(const lithogui::DrawList& dl);

 private:
  void fillRect(const lithogui::Rect& r, lithogui::Color c);
  void drawText(lithogui::Vec2 pos, const std::string& text, lithogui::Color c, float scale);
  void drawLine(lithogui::Vec2 a, lithogui::Vec2 b, lithogui::Color c, float thickness);
  void drawImage(const lithogui::Rect& r, int textureId, lithogui::Color tint);
  void pushClip(const lithogui::Rect& r);
  void popClip();

  HudClip clip() const { return clips_.empty() ? HudClip{} : clips_.back(); }
  HudLayer& layer_;
  ImageResolver images_;
  IconResolver icons_;
  std::vector<HudClip> clips_;
  // Textured icons: (draw index in layer_.draws, atlas entry) pairs
  // emitted by this render() call; render() rewrites their UVs from
  // the entries once the atlas settles for the layer's stamp.
  std::vector<std::pair<size_t, int>> iconFixups_;
  int iconEmitted_ = 0;
};

/// Box-filters `src` down to `size` x `size` (alpha-weighted colour), for HUD icons.
Image downsampleIcon(const Image& src, int size = 16);
/// Drops the fitted-size icon copies (call when the icon images are reloaded).
void invalidateHudIconCache();

}  // namespace litholattice
