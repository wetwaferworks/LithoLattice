// Immutable snapshot of a sub-chunk plus a 1-cell border (18^3 ids + state bytes), taken
// by the thread that owns the ChunkMap. Mesh workers read only this, never the live map.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "litholattice/world/chunk_map.h"

namespace litholattice {

constexpr int kMeshPad = kChunkSize + 2;  // 18
constexpr int kMeshPadVolume = kMeshPad * kMeshPad * kMeshPad;
inline int meshPadIndex(int x, int y, int z) { return (y * kMeshPad + z) * kMeshPad + x; }
/// P8 (Archie Q3): what the apron reads where a neighbor COLUMN is not resident.
/// Air = legacy (tools, captures, tests); Clamp = the sub-chunk's own edge cell.
enum class MeshEdge : uint8_t { Air = 0, Clamp = 1 };

struct MeshInput {
  int cx = 0, sy = 0, cz = 0;
  uint32_t epoch = 0;   // dimension load generation (stale results are dropped)
  uint64_t seq = 0;     // newer snapshot of the same sub-chunk wins
  // Cortana 2026-10-15 1350 - handshake-coupled meshing, phase-boundary
  // publish (idea: JD; refinement: Cortana): every sub-chunk snapshotted by
  // one cSimThread::flushDirty() (one settled post-phase world state) shares
  // a batch id; batchSize = how many. 0 = not part of a publish batch.
  uint64_t batch = 0;
  uint32_t batchSize = 0;
  BlockId id[kMeshPadVolume];
  CellState st[kMeshPadVolume];
  uint8_t mat[kMeshPadVolume];  // per-voxel 8-bit material palette index (PBR chunk materials);
                               // 0 when the source cell has none
  // Column material palette for the BinaryQuad path (PBR phase 2, Worker 3): matPalette[i]
  // maps a mat[] byte to a compact cMaterialReg ID. Copied at snapshot time so mesh
  // workers never touch the live ChunkMap (same immutability contract as id[]/st[]/mat[]).
  // matPaletteSize == 0 (old save, no MATS payload) means every byte resolves to ID 0.
  uint16_t matPalette[256]{};
  size_t matPaletteSize = 0;
  // 2026-10-25 (JD, Posts): text of this sub-chunk's own cells (SubChunk::index -> text), copied
  // at snapshot time like the palette. Empty for almost every sub-chunk.
  std::vector<std::pair<uint16_t, std::string>> text;
  const std::string* textAt(int x, int y, int z) const {
    const uint16_t k = uint16_t(SubChunk::index(x, y, z));
    for (const auto& t : text) if (t.first == k) return &t.second;
    return nullptr;
  }
};

/// Fill `out` with the snapshot (out-param form, for pooled callers that recycle the
/// ~24 KB MeshInput instead of make_shared-ing one per snapshot).
/// `reg` translates the StableId voxel storage to the BlockId currency of MeshInput
/// (resolved once per sub-chunk palette, then O(1) per voxel).
void snapshotForMeshingInto(const ChunkMap& w, const cBlockReg& reg, int cx, int sy, int cz,
                            uint32_t epoch, uint64_t seq, BlockId belowWorld, MeshInput& out,
                            MeshEdge edge = MeshEdge::Air);

/// Snapshot of sub-chunk (cx, sy, cz) with its border. Cells below y = 0 read as `belowWorld`
/// (the dimension's fill block; a solid one keeps the world's bottom faces from being meshed).
std::shared_ptr<MeshInput> snapshotForMeshing(const ChunkMap& w, const cBlockReg& reg, int cx, int sy,
                                              int cz, uint32_t epoch, uint64_t seq,
                                              BlockId belowWorld = kAir, MeshEdge edge = MeshEdge::Air);

}  // namespace litholattice
