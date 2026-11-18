// Software-renderer anti-aliasing modes (JD 2026-10-13).
//
// The software renderer is the engine's equalizer lane — it must run from
// retro hardware and basic Chromebooks up to state-of-the-art GPU-less
// CPUs — so AA is a LADDER of named modes behind `render.aa`, not one
// technique. All modes are post/raster-output stages on the CPU
// framebuffer: purely presentational, zero sim interaction.
//
//   off     - no AA. The baseline; also JD's reserved default until he
//             picks from the capture sets.
//   fxaa    - FXAA-class post filter (Lottes lineage): local-contrast
//             gate, subpixel term, edge-direction endpoint search,
//             coverage offset. Cheapest real AA; softens thin lines.
//   cmaa    - FLAGSHIP: conservative morphological coverage restoration
//             (MLAA -> SMAA -> CMAA2 lineage, clean-room variant). Detects
//             edges (luma + the renderer's depth buffer), reconstructs
//             the edge line through each edge pixel from the run's ends
//             and crossing edges, computes the pixel's TRUE coverage by
//             the far side analytically (no lookup textures), and blends
//             by that coverage. Conservative gates keep it off texture
//             detail, so thin trace lines survive.
//   ssaa2x  - true 2x supersampling: the renderer rasterizes at 2x in
//             both axes and box-resolves. Quality reference; ~4x fill.
//
// Hardware classes (JD's inclusion rationale): fxaa = Chromebook / retro
// floor; cmaa = mainstream CPU without GPU; ssaa2x = strong CPU,
// quality-first. See Software-Renderer-AA-2026-10-13.md for the survey,
// per-mode costs, and the clean-room/license notes.
#pragma once

#include <cstdint>
#include <vector>
#include "litholattice/render/software_renderer.h"  // SoftwareFramebuffer

namespace litholattice {

enum class SoftwareAaMode : int { Off = 0, Fxaa = 1, Cmaa = 2, Ssaa2x = 3 };

/// Stable cvar spellings for render.aa ("off" | "fxaa" | "cmaa" | "ssaa2x").
const char* softwareAaModeName(SoftwareAaMode mode);
bool softwareAaModeFromName(const char* name, SoftwareAaMode& out);

/// Reusable scratch for the post passes (avoids per-frame allocation).
struct SoftwareAaScratch {
  std::vector<uint32_t> src;     ///< FXAA: pending blended pixels (indexed); also CMAA/bloom scratch
  std::vector<float> luma;       ///< per-pixel luma (cmaa edge detection)
  std::vector<uint16_t> lumaQ;   ///< integer luma 0-255 (fxaa, fixed-point path)
  std::vector<uint8_t> edge;     ///< FXAA: 1 = pending commit into fb (grow-only)
  std::vector<uint8_t> edgeR;    ///< cmaa: edge between (x,y) and (x+1,y)
  std::vector<uint8_t> edgeB;    ///< cmaa: edge between (x,y) and (x,y+1)
  std::vector<float> acc;        ///< cmaa: 4 floats per pixel (rgb, weight)
  std::vector<float> bloomA, bloomB;  ///< bloom: quarter-res RGB bright pass + blur ping-pong
};

/// Grow FXAA scratch (never shrinks) and compute integer luma. Does not copy the
/// colour buffer: applyFxaaRows gathers from fb and parks blends in scratch until
/// commitFxaaRows (so concurrent bands never read a half-updated frame).
void prepareFxaa(const SoftwareFramebuffer& fb, SoftwareAaScratch& scratch);

/// Integer luma for rows [y0, y1). Used to thread prepareFxaa over the band pool.
void prepareFxaaRows(const SoftwareFramebuffer& fb, SoftwareAaScratch& scratch, int y0, int y1);

/// FXAA-class post filter on rows [y0, y1). Call prepareFxaa first. Gathers from fb,
/// writes pending pixels into scratch (fb is untouched until commitFxaaRows).
/// Geometry-edge gate (JD 2026-11-17): a pixel is filtered only if its 3x3 neighbourhood
/// has a depth step and/or a primitive-id step. Texture interiors stay bit-identical.
/// Sky (depth 0xFFFF) vs any written depth is always an edge; other depth jumps use a
/// relative leftover-far-code test, because a fixed 0.0025 threshold misses far silhouettes.
/// Math is fixed-point. depth and/or prim may be null; if both are null the pass is a no-op
/// (no luma-only filtering).
void applyFxaaRows(SoftwareFramebuffer& fb, SoftwareAaScratch& scratch,
                   const uint16_t* depth, const uint16_t* prim, int y0, int y1);

/// Scatter pending FXAA blends into fb for rows [y0, y1). Call after every band has
/// finished applyFxaaRows (disjoint row writes, no locks).
void commitFxaaRows(SoftwareFramebuffer& fb, SoftwareAaScratch& scratch, int y0, int y1);

/// Full-frame convenience: prepareFxaa + applyFxaaRows + commitFxaaRows (1 thread).
void applyFxaaPost(SoftwareFramebuffer& fb, SoftwareAaScratch& scratch,
                   const uint16_t* depth = nullptr, const uint16_t* prim = nullptr);

/// Cortana 2026-10-16 (render.bloom): bright-pass (luma threshold, sky excluded via depth) at
/// quarter resolution, separable 9-tap blur, additive composite. Presentational only.
void applyBloomPost(SoftwareFramebuffer& fb, const uint16_t* depth, SoftwareAaScratch& scratch);

/// Conservative morphological coverage AA, in place on fb. `depth` is the
/// renderer's NDC-z depth buffer at fb resolution (may be null: luma-only
/// edge detection, weaker on same-color geometry steps).
void applyCmaaPost(SoftwareFramebuffer& fb, const uint16_t* depth, SoftwareAaScratch& scratch);

/// 2x box resolve: hi (2W x 2H) -> out (W x H), exact 2x2 average.
void resolveSsaa2x(const SoftwareFramebuffer& hi, SoftwareFramebuffer& out);

}  // namespace litholattice
