#pragma once
#include "litholattice/world/chunk_map.h"

namespace litholattice {
// Voxel DDA (Amanatides & Woo). Returns the first non-air cell and the cell in front of it.
/// With `blocks`, kFlatPick cells are hit only through their flat plate (look past a wire).
/// 2026-10-25 (JD): with `modelPick` (the player's look / target ray only) a cell whose game pick
/// shape is set (setPickShapeFn) is hit only through its own boxes, so the player aims past the
/// empty part of a partial block's cell. The camera boom and tests keep cube cells.
bool raycast(const ChunkMap& w, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, int hit[3], int prev[3], const cBlockReg* blocks = nullptr, bool modelPick = false);
/// One pick box in cell-local block units ([0, 1] per axis).
struct PickBox { float lo[3], hi[3]; };
/// Game hook: writes up to maxBoxes boxes for cell (x, y, z) holding `b` and returns the count, or
/// -1 for "the full cell" (cubes and anything without a model). Must be pure (called on the sim thread).
using PickShapeFn = int (*)(const ChunkMap& w, const cBlockReg& blocks, int x, int y, int z, BlockId b,
                            PickBox* out, int maxBoxes);
void setPickShapeFn(PickShapeFn fn);
/// The pick box of a kFlatPick cell: [x, x + 1] x [y0, y1] x [z, z + 1].
void flatPickRange(const ChunkMap& w, const cBlockReg& blocks, int x, int y, int z, float& y0, float& y1);
}  // namespace litholattice
