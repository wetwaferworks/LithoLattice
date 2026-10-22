#include "litholattice/render/block_materials.h"
#include <algorithm>
#include "litholattice/render/block_appearance.h"

namespace litholattice {

BlockMaterials::BlockMaterials(size_t n) {
  faces_.assign(n, {});
  leafSlot_.assign(n, 0);
  wire_.assign(n, {});
  swatch_.assign(n, {1.f, 0.f, 1.f});
}

BlockMaterials BlockMaterials::build(const BlockRegistry& reg, const TextureRegistry& textures, const BlockModelSet& models,
                                     std::vector<std::string>* warnings, const IBlockAppearance* appearance) {
  BlockMaterials m(reg.size());
  auto warn = [&](std::string w) { if (warnings) warnings->push_back(std::move(w)); };

  // Animations the shader can play (registry order, up to the shader's table size).
  for (const AnimatedTexture& a : textures.animations()) {
    if (m.animations_.size() == kMaxAnimations || m.frames_.size() + a.timing.sequence.size() > kMaxAnimationFrames) {
      warn("animation '" + a.name + "' exceeds the shader's animation table; drawn static");
      break;
    }
    m.animations_.push_back({int(m.frames_.size()), int(a.timing.sequence.size()), float(a.timing.frameMs)});
    m.frames_.insert(m.frames_.end(), a.timing.sequence.begin(), a.timing.sequence.end());
  }
  auto material = [&](const FaceRef& r) {
    FaceMaterial fm{uint16_t(r.texture.value)  /* Cortana 2026-10-15 1628: CA-03 */, r.uv, FaceMaterial::kNotAnimated};
    if (r.animation.valid() && r.animation.value < m.animations_.size()) fm.animation = uint8_t(r.animation.value);
    return fm;
  };

  {
    const auto all = textures.textures();
    for (size_t i = 1; i < all.size() && i < 65536; ++i)  // WaferBeta: was < 256 (pre CA-03 8-bit layers); 330 textures now
      if (!all[i].name.empty()) m.named_.emplace(all[i].name, FaceMaterial{uint16_t(i), {}, FaceMaterial::kNotAnimated});
    if (const FaceMaterial* d = m.named("debug_wire_digits_mask")) m.bindings_.debugDigits = d->layer;
    // JD Vulkan push (Cortana 2026-10-16): normal / glow maps by naming convention.
    m.fxMap_.assign(all.size(), 0);
    for (size_t i = 1; i < all.size() && i < 65536; ++i) {
      const std::string& n = all[i].name;
      if (n.empty()) continue;
      const FaceMaterial* nm = m.named(n + "_normal");
      if (!nm) nm = m.named(n + "_etched_normal");
      const FaceMaterial* gm = m.named(n + "_glow_mask");
      m.fxMap_[i] = (gm ? int(gm->layer) << 16 : 0) | (nm ? int(nm->layer) : 0);
    }
  }
  for (size_t id = 1; id < reg.size(); ++id) {
    const BlockDef def = reg.def(BlockId(id));
    if (def.tex.empty()) continue;
    const BlockModel* model = models.find(def.tex);
    if (!model) {
      warn("no block model for '" + std::string(def.tex) + "' (magenta checker)");
      continue;
    }
    for (size_t f = 0; f < kFaceSlotCount; ++f)
      for (int active = 0; active < 2; ++active)
        m.faces_[id][f][size_t(active)] = material(model->face(FaceSlot(f), active != 0));
    for (size_t p = 0; p < kWirePieceCount; ++p) m.wire_[id][p] = material(model->wirePiece(WirePiece(p)));

    if (appearance) appearance->bindMaterials(def, *model, m.bindings_);
    // 2026-10-16 (Wafer beta art): "tint:<texture>" masks -> kShadeTraceTint slots (shared table).
    for (const auto& [role, maskTex] : model->masks) {
      if (role.rfind("tint:", 0) != 0 || maskTex.missing()) continue;
      const FaceMaterial* tex = m.named(role.substr(5));
      if (!tex) { warn("tint mask '" + role + "': no texture named '" + role.substr(5) + "'"); continue; }
      if (m.tintSlotOfLayer_.size() <= tex->layer) m.tintSlotOfLayer_.resize(size_t(tex->layer) + 1, 0);
      if (m.tintSlotOfLayer_[tex->layer]) continue;  // shared texture: first mask wins
      size_t slot = 0;
      while (slot < MaterialBindings::kTintSlots && m.bindings_.tintMask[slot]) ++slot;
      if (slot == MaterialBindings::kTintSlots) { warn("tint mask '" + role + "': more than 32 tint masks"); continue; }
      m.bindings_.tintMask[slot] = int(maskTex.value);
      m.tintSlotOfLayer_[tex->layer] = uint8_t(slot + 1);
    }
    // 2026-10-16 (JD leaf pass): leaves with a "glow" mask get a twinkle slot (first 4).
    if (def.flags & kLeaves) {
      const TextureId g = model->mask("glow");
      if (!g.missing()) {
        int slot = 0;
        for (int k = 0; k < 4 && !slot; ++k)
          if (m.bindings_.leafGlow[size_t(k)] == int(g.value) || m.bindings_.leafGlow[size_t(k)] == 0) slot = k + 1;
        if (slot) { m.bindings_.leafGlow[size_t(slot - 1)] = int(g.value); m.leafSlot_[id] = uint8_t(slot); }
        else warn("leaf glow '" + std::string(def.tex) + "': more than 4 glow masks, drawn without twinkle");
      }
    }

    // HUD swatch.
    if (model->isWire() && model->palette.valid()) {
      const auto& colors = textures.palette(model->palette).colors;
      if (!colors.empty()) {
        const auto& c = colors.back();
        m.swatch_[id] = {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f};
        continue;
      }
    }
    const TextureId top{model->isWire() ? model->wirePiece(WirePiece::Straight).texture : model->face(FaceSlot::Top, true).texture};
    m.swatch_[id] = textures.texture(top).image.averageColor();
  }
  return m;
}

}  // namespace litholattice
