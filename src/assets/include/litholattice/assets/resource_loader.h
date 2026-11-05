// Builds the CPU-side resource set (textures, animations, palettes, block models) from a pack
// stack. Definitions from every pack's resources.json are merged by name (higher pack wins), and
// every file is then read from the highest pack that has it, so a pack can override a single
// PNG without repeating any JSON. Schema: docs/RESOURCE_PACKS.md.
#pragma once
#include <cstdint>
#include <list>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "litholattice/assets/block_model.h"
#include "litholattice/assets/resource_pack.h"
#include "litholattice/assets/texture_registry.h"
#include "litholattice/core/atomic_gate.h"

namespace litholattice {

struct ResourceSet {
  cTextureReg textures;
  BlockModelSet models;
  std::vector<std::string> warnings;  ///< bad entries, missing files (all non-fatal)
};

/// Never fails: anything broken becomes the missing texture plus a warning.
ResourceSet loadResources(const cPackStack& stack);

/// IEEE CRC32 (the provenance hash). The assets module links neither storage nor world,
/// which carry their own copies; this one is local to pack provenance.
uint32_t lithoCrc32(std::span<const uint8_t> bytes);

/// Litho certificate: provenance marker on a pack file entry, stamped at pack-authoring
/// time as `"provenance": {"origin": "litho-generated", "crc32": "0x..."}` over the source
/// file bytes. Scope (P3-3, Appendix E L8): verified for every FILE-BACKED entry —
/// textures, animations and palettes. Model defs carry no source file, so they
/// cannot carry a verifiable marker. This is provenance ("engine-made vs
/// third-party"), NOT the mod-signing PKI (separate roadmap): CRC32 is a
/// non-cryptographic label; it tells content apart, it never gates loading.
enum class Provenance { Absent, Verified, Mismatch };
/// Verifies the "provenance" object on a file-backed manifest def. Absent when the entry
/// carries no marker (ordinary third-party content -- the default, no warning). Verified
/// when the CRC32 of the source file bytes matches the marker. Mismatch when the marker
/// is present but wrong, malformed, of unknown origin, or the source bytes can't be read;
/// `err` describes the failure. Callers treat Absent and Mismatch alike: third-party.
Provenance verifyTextureProvenance(const cPackStack& stack, const std::string& rel,
                                  const Json& def, std::string& err);

/// Raster cache for SVG textures (Litho SVG): one rasterization per (CANONICAL
/// absolute path, tile size, file size, mtime), so repeated pack loads and UI
/// reuse don't re-rasterize while hot-reloaded files (mtime/size change) do.
/// The resource loader uses a shared instance; UI code may hold its own.
/// P3-3 (Appendix E L7): the cache is BOUNDED (kMaxEntries, LRU eviction),
/// MUTEX-guarded (all current callers are main-thread, but the shared
/// instance must not rely on that), and keyed by the canonical path (the
/// old raw dir_/rel key rasterised the same file twice under different
/// relative spellings). KeyHash covers path + tile + size + mtime.
struct SvgRasterCache {
  static constexpr size_t kMaxEntries = 128;
  std::optional<Image> fetch(const std::string& absPath, int tileSize);
  void store(const std::string& absPath, int tileSize, Image img);
  void clear();
  size_t size() const;
 private:
  struct Key {
    std::string path;
    int tile = 0;
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    uint64_t digest = 0;  ///< Cortana 2026-10-25: content FNV-1a; mtime alone misses same-size rewrites within the FS timestamp tick
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept;
  };
  static Key makeKey(const std::string& absPath, int tileSize);
  struct Entry {
    Image image;
    std::list<Key>::iterator lru;  // position in lru_ (front = most recent)
  };
  mutable litholattice::cAtomicGate mutex_;
  std::unordered_map<Key, Entry, KeyHash> map_;
  mutable std::list<Key> lru_;  // front = most recently used
};

/// Clears the loader's shared SVG raster cache (test hook).
void clearSharedSvgRasterCache();

/// Bounded LRU cache of DECODED raster images (CREW TILE, shake 2026-10-18):
/// PNG textures used to be decoded from disk on EVERY resource load -- one
/// full cResourceMgr reload performs ~490 Image::load calls over the
/// capture pack stack (measured census, shake report), and every hot reload
/// (F9) pays it again, while SVG rasters already had this exact cache. This
/// is the PNG twin of SvgRasterCache, same discipline: keyed by the
/// CANONICAL absolute path + file size + mtime, so a hot-reloaded (edited)
/// file misses and re-decodes; failed decodes are never stored, so a fixed
/// file loads on the next attempt. Bounded by entry count AND total decoded
/// bytes, LRU eviction; mutex-guarded (same rationale as SvgRasterCache).
/// Values are the exact Images Image::load produces -- a hit returns a copy
/// of the stored decode, byte-identical by construction.
struct TextureDecodeCache {
  static constexpr size_t kMaxEntries = 512;
  static constexpr size_t kMaxBytes = 64ull * 1024 * 1024;  // decoded RGBA
  std::optional<Image> fetch(const std::filesystem::path& file);
  void store(const std::filesystem::path& file, Image img);
  void clear();
  size_t size() const;
  size_t bytes() const;
  uint64_t hits() const;
  uint64_t misses() const;
 private:
  struct Key {
    std::string path;
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    uint64_t digest = 0;  ///< Cortana 2026-10-25: content FNV-1a; mtime alone misses same-size rewrites within the FS timestamp tick
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept;
  };
  static Key makeKey(const std::filesystem::path& file);
  struct Entry {
    Image image;
    size_t bytes = 0;
    std::list<Key>::iterator lru;  // position in lru_ (front = most recent)
  };
  mutable litholattice::cAtomicGate mutex_;
  std::unordered_map<Key, Entry, KeyHash> map_;
  mutable std::list<Key> lru_;  // front = most recently used
  size_t bytes_ = 0;
  uint64_t hits_ = 0, misses_ = 0;
};

/// The process-wide decode cache shared by the resource loader and the
/// game's icon loads.
TextureDecodeCache& sharedTextureDecodeCache();

/// Clears the shared decode cache (test hook, mirrors clearSharedSvgRasterCache).
void clearSharedTextureDecodeCache();

/// Image::load through the shared decode cache: a hit returns the stored
/// decode (identical bytes); a miss decodes, stores on success, and returns
/// the fresh image. Failures behave exactly like Image::load (nullopt + err)
/// and are never cached.
std::optional<Image> loadTextureFileCached(const std::filesystem::path& file,
                                           std::string* err);

/// Current resources.json format version.
constexpr int kResourceFormatVersion = 1;

}  // namespace litholattice
