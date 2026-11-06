// cHudUiRenderer: consumes a Litho GUI draw list (lithogui::DrawList) and appends
// ordered, CPU-clipped HudDraws to a HudLayer, so both backends (gl33 and Vulkan)
// render the GUI through buildHudVertices with no backend-specific GUI code.
// Images resolve through callbacks (the HUD has no texture path yet): a texture id
// either maps to a pixel-art icon image drawn per pixel, or to a colour swatch.
#pragma once
#include <functional>
#include <optional>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/render/renderer.h"
#include "litholattice/ui/litho_gui.h"

namespace litholattice {

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
  void drawImage(const lithogui::Rect& r, int textureId, float imageAlpha);
  void pushClip(const lithogui::Rect& r);
  void popClip();

  HudClip clip() const { return clips_.empty() ? HudClip{} : clips_.back(); }
  HudLayer& layer_;
  ImageResolver images_;
  IconResolver icons_;
  std::vector<HudClip> clips_;
};

/// Box-filters `src` down to `size` x `size` (alpha-weighted colour), for HUD icons.
Image downsampleIcon(const Image& src, int size = 16);
/// Drops the fitted-size icon copies (call when the icon images are reloaded).
void invalidateHudIconCache();

}  // namespace litholattice
