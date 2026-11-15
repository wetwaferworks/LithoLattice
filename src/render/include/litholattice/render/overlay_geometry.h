// CPU-side overlay geometry shared by the render backends: HUD quads (rects + font glyphs), the
// HUD font atlas, the block-highlight outline and the crosshair. Backends only upload and draw.
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "litholattice/core/math.h"
#include "litholattice/core/tick_profile.h"
#include "litholattice/render/renderer.h"

namespace litholattice {

/// One HUD vertex in pixels (origin top-left). u < 0 marks a solid rectangle (no font lookup).
/// tex (textured icons, Phase B 2026-11-09): 0 = the font/solid behaviour
/// above; 1 = icon mode... sample the layer's icon atlas RGBA at (u, v),
/// multiplied by the vertex colour as tint. An explicit field, not a
/// sentinel band of u, so no magic-number contract crosses the backends.
struct HudVertex { float x, y, u, v, r, g, b, a, tex; };
static_assert(sizeof(HudVertex) == 9 * sizeof(float), "HUD vertex layout");

/// The baked HUD font as a single-channel (R8) atlas: 16 x 6 glyph cells.
struct HudFontAtlas {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;  ///< width * height coverage values, row-major
};

/// Builds the font atlas from the baked glyph table (font_data.h).
HudFontAtlas buildHudFontAtlas();

/// Appends the HUD as triangles (6 vertices per quad): rects first, then each text with a drop
/// shadow, then the UI draws in order (clipped). Clears `out` first.
/// next-build-2: when LL_TICK_PROFILE is on, the call's duration lands in
/// tickprof::hudExpandNs() (the GUI profile's expand phase).
void buildHudVertices(const HudLayer& hud, std::vector<HudVertex>& out);

/// Content-generation key for the cached expansion below (GUI lag fix,
/// item 2, 2026-11-09). One instance lives beside each backend's
/// persistent HUD vertex buffer: `key` is the content fingerprint of the
/// layer that buffer currently holds the expansion of, `valid` is false
/// until the first expansion lands. The fingerprint covers every field
/// buildHudVertices reads (layer scale; every rect, text and draw field;
/// floats by bit pattern, strings by length and bytes), so two layers
/// share a key only when their expansions are identical... the cache is
/// therefore correct under ANY producer mutation discipline, bumped or
/// not: a mutation that reaches the layer changes the fingerprint on the
/// way in. (The facade's paint generation never crosses into the engine
/// and the layer carries no stamp; keying on content is what makes the
/// cache honest engine-side. See the fix report's mutation-path audit.)
struct HudVertexCache {
  uint64_t key = 0;
  bool valid = false;
};

/// Content fingerprint of a layer (GUI lag fix item 2's key pass,
/// exposed for the split-stream path below): a pure function of the
/// layer's content; identical content, identical key.
uint64_t hudContentKey(const HudLayer& hud);

/// Split-stream cached expansion state (GUI draw-cost program, cure 1,
/// 2026-11-10): one instance per (backend, stream). Pairs the content
/// cache with the producer generation the cached expansion was built
/// under, so a producer that stamps generations lets the backend skip
/// even the fingerprint pass while the producer guarantees the layer
/// is unmutated at that generation (the facade's generation-gated
/// caches are exactly that guarantee; the fingerprint still runs on
/// any generation change, so a stamp lie can only cost a re-expansion,
/// never a stale frame... see hudVertsForStream).
struct HudStreamCache {
  HudVertexCache content;
  uint64_t builtGeneration = 0;  // 0 = no producer stamp seen yet
};

/// The split-stream entry point (cure 1). Behaviour:
///  - generation != 0 and == state.builtGeneration and the content
///    cache is valid: the producer guarantees this exact layer content
///    was already expanded into `out`; touch nothing, pay nothing
///    (not even the fingerprint). Returns false.
///  - otherwise run the item-2 content cache: fingerprint match pays
///    the fingerprint only; a miss expands fully. The producer stamp
///    is (re)recorded either way. Returns true iff an expansion ran.
/// `out` obeys the same contract as buildHudVerticesCached: one
/// persistent buffer per (backend, stream), never copied per frame.
bool hudVertsForStream(const HudLayer& hud, uint64_t generation, HudStreamCache& state, std::vector<HudVertex>& out);

/// Cached form of buildHudVertices for the per-frame backend path. When
/// hud's content fingerprint matches `cache`, `out` already holds this
/// layer's expansion and is left untouched (returns false; the frame
/// pays only the fingerprint pass, which is also what lands in
/// tickprof::hudExpandNs() for the frame). Otherwise expands into `out`,
/// records the fingerprint, and returns true. `out` must be the same
/// buffer across calls for a hit to mean anything: the backends pass
/// their per-instance member buffer, which is also the cache's identity
/// (one cache per presented layer stream). The expanded data is shared
/// by staying in place in that buffer, never copied per frame.
/// Threading: the cache is exactly as thread-confined as the buffer it
/// keys (one backend instance, its own present path... the same
/// confinement the uncached expansion already relied on). No locking is
/// needed, and none is added.
bool buildHudVerticesCached(const HudLayer& hud, HudVertexCache& cache, std::vector<HudVertex>& out);

/// Per in-flight-slot residency record for one HUD vertex range (B9
/// hotbar-render fix, 2026-11-11). A backend that keeps one overlay
/// buffer per frame in flight must refill a slot's range whenever the
/// slot does not hold EXACTLY the current content: same content key,
/// same vertex count, same offset. Count alone is not enough: a
/// selection or hover change swaps colours/frames without moving the
/// vertex count, and a "fresh expansion" flag is true for one frame
/// only, so only the slot recorded on that frame got the new bytes and
/// the other slot kept presenting the old selection (frames alternate
/// old/new: the flickering selected-hotbar quad). `count == SIZE_MAX`
/// means "holds nothing" (first use, or a freshly grown buffer).
struct HudSlotRange {
  uint64_t key = 0;
  size_t count = SIZE_MAX;
  size_t at = SIZE_MAX;
  void reset() { key = 0; count = SIZE_MAX; at = SIZE_MAX; }
};

/// True iff the slot's range must be (re)copied to hold (contentKey,
/// count) at offset `at` (in vertices). Records the new residency
/// either way: the caller copies when this returns true. Pure, so the
/// two-slot alternation is unit-testable without a device.
bool hudSlotRangeNeedsCopy(HudSlotRange& r, uint64_t contentKey, size_t count, size_t at);

/// Outline of block (x, y, z) as 12 line segments (24 endpoints), slightly inflated.
std::array<Vec3, 24> blockOutlineLines(int x, int y, int z);

/// Crosshair as 2 line segments in normalized device coordinates (y up), aspect corrected.
std::array<Vec3, 4> crosshairLines(int width, int height);

/// D9 2026-11-12 (JD: creative-inventory flicker, the world flashing
/// through the GUI for single frames while a carried stack sweeps the
/// palette). FrameParams::hudStatic / hudDynamic BORROW the producer's
/// layers "for the duration of the frame call only" (renderer.h). The
/// LL_THREEWAY render workers (main.cpp RenderWorker / SwRenderWorker;
/// thread layout 8, the default, turns LL_THREEWAY on) keep the job
/// past that call: main returns from submit() and paints the NEXT frame
/// into the very same layers in place (cUiRoot clears paintCache_ /
/// dynCache_ and re-emits) while the render thread is still expanding
/// the job's layer. A static rebuild racing the expansion presents an
/// empty or partial static stream for one frame. The fix: submit()
/// copies each borrowed stream into storage the job owns, re-copying
/// only when the stream's (pointer, generation) moves (the same
/// identity the backends' hudVertsForStream fast path already trusts),
/// and points the job at the copies. submit() runs only while the
/// worker is idle, so the copies are never written under a render.
struct HudStreamSnapshot {
  HudLayer staticLayer, dynamicLayer;
  const HudLayer* staticSrc = nullptr;
  const HudLayer* dynamicSrc = nullptr;
  uint64_t staticGen = 0, dynamicGen = 0;
  uint64_t staticCopies = 0, dynamicCopies = 0;  ///< copies taken (diagnostics, tests)
};
/// Points job.hudStatic / job.hudDynamic at `own`'s copies of src's
/// streams (null stays null). Everything else in `job` is untouched.
void snapshotHudStreams(const FrameParams& src, FrameParams& job, HudStreamSnapshot& own);

}  // namespace litholattice
