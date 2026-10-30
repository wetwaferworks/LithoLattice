// Voxel mesher: MeshInput snapshot (world) -> ChunkMesh (render). Pure function, runs on
// worker threads. Shapes come from the game's cBlockReg, texture layers from BlockMaterials,
// and everything game-specific (custom shapes, shader modes) from its IBlockAppearance.
#pragma once
#include <functional>
#include "litholattice/render/renderer.h"
#include "litholattice/world/mesh_input.h"
#include "litholattice/render/trace_strength_buffer.h"

namespace litholattice {
class BlockMaterials;
class IBlockAppearance;

/// Wafer ground-pulse params for one chunk column, as two shader-ready vec4s:
/// p0 = (freqHz, sharpness, amplitude, 0), p1 = (tintR, tintG, tintB, 0).
/// The game fills these from its biome weight vector (see ground_pulse.h).
using PulseParamsFn = std::function<void(int chunkX, int chunkZ, float p0[4], float p1[4])>;

/// Wafer ground-trace params for one chunk column, as two shader-ready vec4s:
/// t0 = (density, speed, amplitude, dirMix), t1 = (tintR, tintG, tintB, 0).
/// The game fills these from the same biome weight vector as the pulse
/// (see ground_traces.h).
using TraceParamsFn = std::function<void(int chunkX, int chunkZ, float t0[4], float t1[4])>;

/// What the mesher reads besides the snapshot. All of it is read-only and shared by workers.
///
/// DEFINITIONS CONTRACT (P3-3, Appendix D M4): unlike the voxel data —
/// which arrives as the immutable MeshInput snapshot — block definitions
/// are read LIVE from `blocks` on worker threads. That is safe per-read
/// (the registry hands out defs by value under a shared lock), and
/// meshSubChunk guards the one remaining hazard, a refreshStaged() landing
/// mid-mesh, with a definitionEpoch before/after check + one re-mesh.
/// `materials` and `appearance` carry no epoch: callers must not mutate
/// them while meshes are in flight (they are built at load and treated as
/// immutable thereafter — the same standing contract as before).
struct MeshContext {
  const cBlockReg& blocks;
  const BlockMaterials& materials;
  const IBlockAppearance* appearance = nullptr;  ///< nullptr: plain textured shapes
  const TraceStrengthBuffer* strengths = nullptr;  ///< Step 1: slot per trace cell (null = bake strength)
  PulseParamsFn pulseParams = nullptr;  ///< null = meshes carry zero pulse params (no pulse)
  TraceParamsFn traceParams = nullptr;  ///< null = meshes carry zero trace params (no traces)
  // Cortana 2026-10-15 1705: greedy meshing [ENGINE]
  /// Merge coplanar, same-look full-cube faces into larger quads (u/v then run
  /// past 1 and tile). Only for backends that wrap UVs per tile
  /// (IRenderer::prefersGreedyMeshes). Env LL_MESH_GREEDY=0/1 overrides for A/B.
  bool greedy = false;
};

/// Builds the face-culled, textured mesh of one sub-chunk. Epoch-guarded:
/// if block definitions change mid-pass, the mesh is rebuilt once against
/// the new set (see MeshContext contract above).
ChunkMesh meshSubChunk(const MeshInput& in, const MeshContext& ctx);

/// 2026-10-16 (JD leaf pass): render.leaves. Fancy (default): kLeaves blocks mesh as
/// two-sided alpha-tested kShadeLeaf faces with no leaf-to-leaf culling. Fast: leaves
/// count as opaque (normal culling + AO) and draw the model's "active" (opaque) texture.
/// Render-only; changing it needs a remesh. Env LL_LEAVES=fast|fancy sets the start value.
void setLeavesFancy(bool fancy);
/// 2026-10-16 1551 (JD): render.bulb_shell - false = Ghostly (default), true = Solid (kShell blocks opaque + emissive).
void setShellSolid(bool solid);
bool shellSolid();
bool leavesFancy();

/// 2026-10-18 (Crew Canopy, F-B): render.foliage_density. 0 = full (the default;
/// today's meshing exactly, bit-identical output), 1 = sparse: in FANCY leaf mode
/// (render.leaves), leaf cubes in columns beyond the near ring (Chebyshev distance
/// from the focus column > 2, the cBenchStreamer LOD's full-detail edge) are thinned
/// by a deterministic density model - a far leaf voxel is kept iff
/// splitmix64(packPos(world pos)) % 1024 < 512 (density 1/2); an unkept far leaf
/// voxel emits nothing, and a leaf-to-leaf interface is culled only when BOTH
/// cells are far and the neighbour is kept, so the kept set's outer shell
/// survives whole (two-sided, as today) while far interior interfaces are
/// culled. The near ring is inviolate: near cells, and every interface touching
/// one, mesh exactly as today.
/// Fast leaf mode already culls interior faces and is unaffected. Render-only;
/// changing it needs a remesh. Env LL_FOLIAGE_DENSITY (0|1) sets the start value.
void setFoliageDensity(int mode);
int foliageDensity();
/// The column the sparse-leaf distance is measured from (the player's column;
/// the game refreshes it as the player/camera moves). Default (0, 0).
void setFoliageFocus(int cx, int cz);
void foliageFocus(int& cx, int& cz);

/// Cortana 2026-10-15 1340 - handshake-coupled meshing verify mode (idea:
/// JD; refinement: Cortana). When on, every meshSubChunk() meshes twice:
/// once from the stable def copy (the shipped path) and once through the
/// locked live registry, and compares 64-bit FNV-1a checksums of the vertex
/// bytes. Default: on in Debug builds (!NDEBUG), off in Release; the game's
/// --verify-mesh turns it on. A pass whose definition epoch moved between
/// the two meshes is skipped (counted, not a mismatch).
void setMeshVerify(bool on);
/// Render pipeline M2 (2026-10-29, Archie step 3): --unified-mesher / LL_UNIFIED_MESHER=1.
/// When on, greedy-eligible faces go through the canonical bitwise merge and every mesh
/// carries ChunkMesh::canonical (L0 face buckets); the PackedVertex stream is decoded from
/// the canonical quads. Off (default, --legacy-mesher) = the legacy path, unchanged.
/// Debug/DebugOpt (and LL_CANON_VERIFY=1 or --verify-mesh) re-mesh on the legacy path and
/// compare quad sets (decode equivalence).
void setUnifiedMesher(bool on);
bool unifiedMesherEnabled();
struct UnifiedMesherStats { uint64_t meshes = 0, checked = 0, mismatches = 0, unpublished = 0, quads = 0, residentBytes = 0, lodQuads = 0, lodFallback = 0, fbVerts = 0, fbChecked = 0, fbMismatches = 0, skirtQuads = 0, splits = 0; };
/// M6: L1/L2 from the registry feed on the game path (LL_CANON_LODS=0 turns it off).
bool canonLodsEnabled();
/// Render pipeline M8 (uploaders): decode one LOD of a canonical mesh to the legacy
/// PackedVertex stream (the CPU reference every uploader is checked against).
/// lod 0 = Packed12 fallback then L0 quads: byte-identical to ChunkMesh::verts in unified
/// mode (slot field 0). lod 1/2 = face buckets, skirts, blend; positions x2/x4, one texture
/// tile per coarse cell (sub-block fallback geometry is not drawn at L1/L2).
/// Returns false (out empty) for an unpublishable mesh or a bad lod.
/// M10 (Archie ruling 2): skirtMask bit f keeps the skirts facing direction f (+X -X +Y -Y
/// +Z -Z); the default keeps all (the mesh-time set). See canonSkirtMask.
bool decodeCanonicalMesh(const CanonicalMesh& cm, int lod, std::vector<PackedVertex>& out, uint8_t skirtMask = 0x3F);
/// M10: draw-side skirt directions for a sub-chunk drawn at `lod`: bit f set iff the
/// neighbour across face f is drawn at another LOD (true exterior LOD boundary).
/// nbrLod[f]: 0..2 = canonical lod, -1 = legacy stream (full detail, = L0), -2 = nothing
/// drawn there (no seam to crack against).
uint8_t canonSkirtMask(int lod, const int nbrLod[6]);
/// M11 (Archie ruling 1): split a mesh whose palette grew past kMaxPalette (up to 2x) in
/// two, in stream order. qSlot / fSlot = the wide palette index of every L0 quad /
/// fallback vertex (the packed 10-bit fields are rewritten). Returns false if the second
/// half overflows too (the mesh is then not publishable).
bool splitCanonicalTwo(CanonicalMesh& cm, const std::vector<uint32_t>& qSlot, const std::vector<uint32_t>& fSlot);
/// Draw-side LOD selection (Archie Q38/Q40): Chebyshev chunk distance d against rings
/// ring1 (L0 -> L1) and ring2 (L1 -> L2), +-1.5 chunk hysteresis (2 in integer chunks)
/// around the current lod (-1 = no current, pick directly).
int selectCanonLod(int d, int current, int ring1, int ring2);
/// LL_CANON_LOD_RINGS="r1,r2" (default 12,24 chunks).
void canonLodRings(int& ring1, int& ring2);
UnifiedMesherStats unifiedMesherStats();
bool meshVerifyEnabled();
struct MeshVerifyStats { uint64_t checked = 0, mismatches = 0, skipped = 0; };
MeshVerifyStats meshVerifyStats();
/// FNV-1a 64 over the mesh's vertex bytes (also used by the verify mode).
uint64_t meshChecksum(const ChunkMesh& m);
}  // namespace litholattice
