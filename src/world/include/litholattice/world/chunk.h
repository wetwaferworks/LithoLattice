// Chunk storage: 16x16 columns, world height 0..767 split into 48 sub-chunks of 16^3
// (all-air sub-chunks are null and skipped by worldgen, meshing and the sim).
#pragma once
#include <array>
#include <map>
#include <string>
#include <cstdint>
#include <memory>
#include "litholattice/world/block.h"
#include "litholattice/world/voxel_palette.h"
#include <optional>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include <mutex>
#include <unordered_map>

namespace litholattice {

constexpr int kChunkSize = 16;
constexpr int kWorldHeight = 768;
constexpr int kSubChunks = kWorldHeight / kChunkSize;  // 48
constexpr int kSubChunkVolume = kChunkSize * kChunkSize * kChunkSize;  // 4096


// Per-cell state: CellState (block.h); its meaning beyond the standard layout is the game's.
// ---- Sub-chunk: width-tagged paletted voxels (Infinite Registry, phase 1) ----
// Index layout y-major: idx = (y << 8) | (z << 4) | x  -> contiguous X runs for SIMD.
// Voxel storage is a VoxelPalette: 1/2/4/8/16/32/64-bit indices chosen from the
// palette size, palette maps index -> StableId (at 64-bit width the index IS the
// StableId). The sub-byte widths are memory-only (audit F3); the BLKS disk codec
// resolves values through the registry and derives its own width, so no save
// byte depends on the in-memory width.
// The engine's BlockId currency is translated at the ChunkMap boundary (get/set keep
// their BlockId signatures); direct sub-chunk readers resolve the palette once per
// sub-chunk. Full game-logic migration to StableIds is WS1 (future work).
// ---- Packed per-voxel material bytes (audit F2) ----
// Variable-width in-memory twin of the MATS disk codec (render/material_payload.h,
// encodeMatBytes): each cell holds an index into a per-sub-chunk table of the distinct
// bytes present (first-appearance order, exactly as the codec builds it), packed at
// w = ceil(log2(distinct)) bits per cell, LSB-first across u64 words, cell i at bits
// [i*w, i*w + w). A uniform sub-chunk -- the common case: no producer assigns material
// bytes in normal play yet, so every byte reads 0 -- stores a single byte and no index
// words at all, replacing the flat 4 KiB array. get()/set() traffic in exactly the byte
// the flat array held; the MATS payload expands/rebuilds via expandTo()/assignDense(),
// so the on-disk bytes are unchanged cell-for-cell.
struct PackedMaterials {
  PackedMaterials() = default;

  /// The material byte of flat cell i (0..kSubChunkVolume-1).
  uint8_t get(size_t i) const {
    if (entries_.empty()) return uniform_;
    if (width_ == 0) return entries_[0];  // total by construction (size-1 tables stay uniform)
    return entries_[readPacked(words_, width_, i)];
  }

  /// Assign the material byte of flat cell i. A first-seen byte extends the distinct
  /// table (appending never renumbers existing cells); if that crosses a power of two
  /// the index width grows and the words are repacked with indices preserved.
  void set(size_t i, uint8_t byte) {
    if (entries_.empty()) {
      if (byte == uniform_) return;
      entries_.push_back(uniform_);
      entries_.push_back(byte);
      width_ = 1;
      words_.assign(wordCount(), uint64_t(0));
      writeIndex(i, 1);
      return;
    }
    size_t pos = 0;
    while (pos < entries_.size() && entries_[pos] != byte) ++pos;
    if (pos == entries_.size()) {
      // A miss means the byte is new, so the table holds < 256 entries here (256
      // entries would cover every possible byte value and the scan would have hit).
      entries_.push_back(byte);
      const int need = bitsFor(entries_.size());
      if (need != width_) repack(need);
    }
    writeIndex(i, pos);
  }

  /// Rebuild from a dense kSubChunkVolume-byte array (MATS load path). Distinct-table
  /// order and width come out exactly as the disk codec chooses them.
  void assignDense(const uint8_t* dense) {
    entries_.clear();
    words_.clear();
    width_ = 0;
    uniform_ = dense[0];
    // int table with a -1 sentinel: a uint8_t table cannot distinguish "unseen" (0xFF)
    // from entry index 255, so the 256th distinct byte would be re-appended on its
    // second occurrence. (The disk codec's uint8_t table has that latent edge too;
    // the in-memory store must not inherit it.)
    std::array<int, 256> entryOf;
    entryOf.fill(-1);
    for (int i = 0; i < kSubChunkVolume; ++i) {
      const uint8_t b = dense[i];
      if (entryOf[b] == -1) {
        entryOf[b] = int(entries_.size());
        entries_.push_back(b);
      }
    }
    if (entries_.size() <= 1) {
      entries_.clear();  // uniform: uniform_ already holds the single byte
      return;
    }
    width_ = bitsFor(entries_.size());
    words_.assign(wordCount(), uint64_t(0));
    for (int i = 0; i < kSubChunkVolume; ++i) writeIndex(size_t(i), entryOf[dense[i]]);
  }

  /// Expand to a dense kSubChunkVolume-byte array (MATS save path).
  void expandTo(uint8_t* out) const {
    for (int i = 0; i < kSubChunkVolume; ++i) out[i] = get(size_t(i));
  }

  /// Bytes this store occupies (object + heap payload), for residency accounting.
  size_t storageBytes() const {
    return sizeof(*this) + words_.size() * sizeof(uint64_t) + entries_.size();
  }

 private:
  static int bitsFor(size_t n) {
    int bits = 0;
    while ((size_t(1) << bits) < n) ++bits;
    return bits;
  }
  size_t wordCount() const { return (size_t(kSubChunkVolume) * size_t(width_) + 63) / 64; }
  uint64_t indexMask() const { return (uint64_t(1) << width_) - 1; }  // width_ <= 8
  static size_t readPacked(const std::vector<uint64_t>& w, int width, size_t i) {
    const size_t bit = i * size_t(width);
    const size_t wi = bit >> 6, sh = bit & 63;
    uint64_t v = w[wi] >> sh;
    if (sh + size_t(width) > 64) v |= w[wi + 1] << (64 - sh);
    return size_t(v & ((uint64_t(1) << width) - 1));
  }
  void writeIndex(size_t i, size_t pos) {
    const size_t bit = i * size_t(width_);
    const size_t wi = bit >> 6, sh = bit & 63;
    const uint64_t m = indexMask();
    words_[wi] = (words_[wi] & ~(m << sh)) | (uint64_t(pos) << sh);
    if (sh + size_t(width_) > 64) {
      const int hiBits = int(size_t(width_) - (64 - sh));
      const uint64_t hiMask = (uint64_t(1) << hiBits) - 1;
      words_[wi + 1] = (words_[wi + 1] & ~hiMask) | (uint64_t(pos) >> (64 - sh));
    }
  }
  void repack(int newWidth) {
    std::vector<uint64_t> old = std::move(words_);
    const int oldWidth = width_;
    width_ = newWidth;
    words_.assign(wordCount(), uint64_t(0));
    for (int i = 0; i < kSubChunkVolume; ++i)
      writeIndex(size_t(i), readPacked(old, oldWidth, size_t(i)));
  }

  std::vector<uint64_t> words_;    // packed indices; empty iff uniform
  std::vector<uint8_t> entries_;   // distinct bytes, first-appearance order; empty iff uniform
  uint8_t uniform_ = 0;            // the single byte when entries_ is empty
  int width_ = 0;                  // index width in bits (0..8) when entries_ is non-empty
};

struct SubChunk {
  VoxelPalette blocks{kSubChunkVolume};
  std::array<CellState, kSubChunkVolume> state{};  // see CellState helpers in block.h
  // PBR chunk materials (phase 2): per-voxel 8-bit index into the column's materialPalette
  // (below), held in PackedMaterials (audit F2): distinct bytes once plus ceil(log2 n)-bit
  // indices -- the MATS codec's scheme -- so a uniform sub-chunk costs a byte instead of
  // 4 KiB. The byte is NOT packed into the block word: BlockId is a dense 16-bit palette
  // index with no spare bits, so packing there would invalidate every existing save and
  // the BLKS palette codec for zero benefit. 0 is the palette's first entry (the built-in
  // default when the column palette is empty).
  PackedMaterials materials;
  uint16_t nonAir = 0;   // 0 => sub-chunk is freed / skipped entirely

  static constexpr int index(int x, int y, int z) { return (y << 8) | (z << 4) | x; }
  StableId at(int x, int y, int z) const { return blocks.at(size_t(index(x, y, z))); }
  void set(int x, int y, int z, StableId id) {
    const size_t i = size_t(index(x, y, z));
    const StableId c = blocks.at(i);
    nonAir += uint16_t((id != kAirStableId) - (c != kAirStableId));
    blocks.set(i, id);
    state[index(x, y, z)] = 0;
    // materials[] is left untouched: the material byte is the caller's to assign (a replaced
    // block keeps its predecessor's material until reassigned; fresh voxels read 0).
  }
  /// 8-bit material palette index of one voxel (indexes ChunkColumn::materialPalette).
  uint8_t materialAt(int x, int y, int z) const { return materials.get(size_t(index(x, y, z))); }
  void setMaterial(int x, int y, int z, uint8_t m) { materials.set(size_t(index(x, y, z)), m); }
  /// Material byte by flat sub-chunk index (mesh snapshot / emissive scan fast paths).
  uint8_t materialFlat(size_t i) const { return materials.get(i); }

  // Transient load scratch (Infinite Registry): the BLKS decoder parks save-time
  // BlockIds here; remapColumn() consumes them into `blocks` (the StableId paletted
  // storage) and clears it. Empty during normal operation.
  std::optional<std::vector<uint16_t>> loadScratch;
};

// ---- Interned per-column material palette (audit F5, half 1) ----
// The palette a column's SubChunk::materials bytes index into: palette[i] is the
// global cMaterialReg ID for material byte i. Phase 2 stored a private
// std::array<uint16_t, 256> (512 B) plus a size in EVERY column, although a
// column's palette is installed whole (MATS load, worldgen setup, tests),
// never edited entry-by-entry in production, and neighbouring columns
// overwhelmingly carry identical palettes. This class keeps the read API
// (size(), operator[]) and replaces the storage with a handle to an
// interned, immutable entry vector: identical contents share one allocation,
// an empty palette costs nothing, and installing a palette replaces the
// handle instead of mutating storage other columns may share. Reads touch
// only the column's own handle (no locking); interning is a cold path
// (load/setup) and is mutex-guarded. The intern table holds weak references,
// so a palette allocation lives exactly as long as some column uses it.
class MaterialPalette {
 public:
  MaterialPalette() = default;

  /// Valid entries (0..256). Size 0 == empty palette: every material byte maps
  /// to global material ID 0, the built-in default.
  size_t size() const { return entries_ ? entries_->size() : 0; }
  bool empty() const { return size() == 0; }

  /// Global cMaterialReg ID at palette position i. Precondition: i < size()
  /// (every caller guards by size, exactly as with the old inline array).
  uint16_t operator[](size_t i) const { return (*entries_)[i]; }

  /// The entries as a contiguous array (nullptr when empty).
  const uint16_t* data() const { return entries_ ? entries_->data() : nullptr; }

  /// Install a palette of n entries (n <= 256: the palette is byte-indexed;
  /// producers validate -- the MATS loader hard-fails above 256 before this is
  /// reachable). n == 0 clears to the empty palette. The entries are interned:
  /// if another column already holds identical contents, the allocation is
  /// shared. Installing never mutates previously installed storage.
  void assign(const uint16_t* entries, size_t n) {
    if (n == 0) {
      entries_.reset();
      return;
    }
    entries_ = intern(entries, n);
  }
  void assign(std::initializer_list<uint16_t> entries) { assign(entries.begin(), entries.size()); }
  void clear() { entries_.reset(); }

  /// Content equality (handles to one interned allocation compare equal fast;
  /// distinct allocations with identical contents also compare equal).
  bool operator==(const MaterialPalette& o) const {
    if (entries_ == o.entries_) return true;
    if (size() != o.size() || empty()) return size() == o.size();
    return std::equal(entries_->begin(), entries_->end(), o.entries_->begin());
  }
  bool operator!=(const MaterialPalette& o) const { return !(*this == o); }

 private:
  using Entries = std::vector<uint16_t>;
  static std::shared_ptr<const Entries> intern(const uint16_t* entries, size_t n) {
    // FNV-1a over the entry words, folded with the length.
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
      h ^= entries[i];
      h *= 1099511628211ull;
    }
    h ^= uint64_t(n) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    struct Table {
      std::mutex mu;
      std::unordered_map<uint64_t, std::vector<std::weak_ptr<const Entries>>> byHash;
    };
    static Table table;
    std::lock_guard<std::mutex> lock(table.mu);
    auto& bucket = table.byHash[h];
    for (auto it = bucket.begin(); it != bucket.end();) {
      if (auto live = it->lock()) {
        if (live->size() == n && std::equal(live->begin(), live->end(), entries)) return live;
        ++it;
      } else {
        it = bucket.erase(it);  // prune dead entries opportunistically
      }
    }
    auto fresh = std::make_shared<const Entries>(entries, entries + n);
    bucket.push_back(fresh);
    return fresh;
  }

  std::shared_ptr<const Entries> entries_;  // null == empty palette
};

struct ChunkColumn {
  int cx = 0, cz = 0;
  // Column eviction bookkeeping (A2 / B-M1, 2026-10-13): `dirty` is set by
  // ChunkMap::set/setState on every mutation of a RESIDENT column, and
  // `generation` counts those mutations. A freshly generated or disk-loaded
  // column starts clean (dirty == false, generation == 0): it is exactly
  // reproducible (worldgen) or already on disk, so eviction may drop it
  // without saving. A dirty column must be saved before it is evicted
  // (world permanence: never drop unsaved state). Direct SubChunk writes
  // that bypass ChunkMap (the signal sim's cached-pointer fast paths) do
  // NOT set this flag -- eviction therefore ALSO treats any column holding
  // signal-sim-indexed cells as save-first (see Game::evictFarColumns).
  bool dirty = false;
  uint64_t generation = 0;
  void markDirty() {
    dirty = true;
    ++generation;
  }
  std::array<std::unique_ptr<SubChunk>, kSubChunks> subs;  // nullptr == all air
  // PBR chunk materials (phase 2): the per-column palette every SubChunk::materials byte
  // indexes into. Byte i -> the global cMaterialReg ID materialPalette[i].
  // materialPalette.size() == 0 (e.g. a column from an old save with no MATS payload) means
  // every byte maps to global ID 0, which initMaterialRegistry() guarantees is the built-in
  // default. The palette is interned (audit F5): see MaterialPalette above -- this member
  // is a 16 B handle where phase 2 paid a 512 B inline array plus a size word per column.
  MaterialPalette materialPalette;
  // 2026-10-25 (JD, Posts): per-cell text (Post panels), keyed by cellTextKey. Empty for nearly
  // every column; saved as the optional "PTXT" payload. ChunkMap::set drops a cell's text when
  // its block changes, so text never outlives the block that owns it.
  std::map<uint32_t, std::string> cellText;
  static constexpr uint32_t cellTextKey(int lx, int y, int lz) { return (uint32_t(y) << 8) | (uint32_t(lz & 15) << 4) | uint32_t(lx & 15); }
};

struct ChunkKey {
  static uint64_t make(int cx, int cz) {
    return (uint64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz));
  }
};

// Packed world-cell (or sub-chunk) coordinate key for hash sets.
// RANGE (P3-2 B-L2): x and z carry 21 bits each with a +2^20 bias, y 10 bits.
// A coordinate is representable exactly iff x,z in [-2^20, 2^20-1] and
// y in [0, 1023]. OUTSIDE that range packPos ALIASES: two different cells
// map to the same key (e.g. x = 2^20 collides with x = -2^20), silently
// merging signal components, light sources and dirty marks for both cells.
// Every dimension's declared bounds must fit inside this range — the
// bounded dimensions do by an order of magnitude (largest: the Cosmic at
// +/-262144) — and the enforcement points are dimension/streamer setup
// (cChunkStreamer refuses out-of-range request centres, loudly, once) and
// packPosInRange() for new call sites. The VOID is the at-risk design:
// its locked "infinite" extent cannot be represented in a packPos key at
// all; before the Void is registered it must be given a finite bound
// inside this range (or a windowed/virtualised coordinate scheme) — see
// game dimensions.h.
constexpr int kPackPosCoordMin = -(1 << 20);        // -1048576
constexpr int kPackPosCoordMax = (1 << 20) - 1;     //  1048575
constexpr int kPackPosYMax = 1023;
inline bool packPosInRange(int x, int y, int z) {
  return x >= kPackPosCoordMin && x <= kPackPosCoordMax && z >= kPackPosCoordMin &&
         z <= kPackPosCoordMax && y >= 0 && y <= kPackPosYMax;
}
inline int64_t packPos(int x, int y, int z) {
  return (int64_t(x + (1 << 20)) << 31) | (int64_t(z + (1 << 20)) << 10) | int64_t(y);  // y: 10 bits
}
inline void unpackPos(int64_t k, int& x, int& y, int& z) {
  y = int(k & 1023); z = int((k >> 10) & ((1 << 21) - 1)) - (1 << 20); x = int(k >> 31) - (1 << 20);
}

}  // namespace litholattice
