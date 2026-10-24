#include "litholattice/assets/resource_pack.h"
#include <array>
#include <fstream>

namespace litholattice {
namespace fs = std::filesystem;
namespace {

/// Pack text files (pack.json / resources.json / shader sources / sidecars)
/// are capped at 16 MiB, enforced DURING the read (P3-3, Appendix E M3):
/// the old slurp read unboundedly, so a hostile multi-GB "manifest" was
/// fully resident before any downstream cap could fire.
constexpr size_t kMaxPackTextBytes = size_t(16) << 20;

std::optional<std::string> slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::string out;
  std::array<char, 64 << 10> chunk;
  while (in) {
    in.read(chunk.data(), std::streamsize(chunk.size()));
    const std::streamsize n = in.gcount();
    if (n > 0) out.append(chunk.data(), size_t(n));
    if (out.size() > kMaxPackTextBytes) return std::nullopt;
  }
  if (in.bad()) return std::nullopt;
  return out;
}

}  // namespace

bool isSafePackPath(std::string_view rel) {
  if (rel.empty() || rel.front() == '/' || rel.front() == '\\' || rel.find(':') != std::string_view::npos) return false;
  // Cortana 2026-10-15 1525 (ENGINE, CloudAtlas #5): pack paths are '/'-separated on every
  // platform. Reject any backslash, Windows reserved device names in any component
  // (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension), and components
  // ending in '.' or ' ' (Windows strips these, so they alias other names).
  if (rel.find('\\') != std::string_view::npos) return false;
  for (size_t b = 0; b <= rel.size();) {
    size_t e = rel.find('/', b);
    if (e == std::string_view::npos) e = rel.size();
    const std::string_view part = rel.substr(b, e - b);
    if (!part.empty()) {
      if (part.back() == '.' && part != "." && part != "..") return false;
      if (part.back() == ' ') return false;
      std::string stem(part.substr(0, part.find('.')));
      for (char& ch : stem) ch = char(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
      if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
      if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' && stem[3] <= '9') return false;
    }
    b = e + 1;
  }
  const fs::path p{std::string(rel)};
  for (const auto& part : p)
    if (part == "..") return false;
  return true;
}

std::optional<ResourcePack> ResourcePack::open(const fs::path& dir, std::string* err) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (err) *err = "pack folder not found: " + dir.generic_string();
    return std::nullopt;
  }
  ResourcePack p;
  p.dir_ = dir;
  p.name_ = dir.filename().string();
  if (const auto text = slurp(dir / "pack.json")) {
    Json j;
    if (Json::parse(*text, j)) p.name_ = j.getStr("name", p.name_);
  }
  if (const auto text = slurp(dir / "resources.json")) {
    std::string perr;
    if (!Json::parse(*text, p.manifest_, &perr)) {
      if (err) *err = (dir / "resources.json").generic_string() + ": " + perr;
      return std::nullopt;
    }
    if (p.manifest_.type != Json::Type::Object) {
      if (err) *err = (dir / "resources.json").generic_string() + ": top level must be an object";
      return std::nullopt;
    }
    p.hasManifest_ = true;
  }
  return p;
}

bool ResourcePack::has(std::string_view rel) const {
  std::error_code ec;
  if (!isSafePackPath(rel)) return false;
  // Canonicalize BOTH the file and the pack root: canonical resolves every
  // symlink, including intermediate directories (pack/textures -> /etc would
  // otherwise escape). The resolved file must stay inside the resolved root
  // and be a regular file.
  const auto canonFile = fs::canonical(path(rel), ec);
  if (ec) return false;
  const auto canonDir = fs::canonical(dir_, ec);
  if (ec) return false;
  auto fi = canonFile.begin();
  for (auto di = canonDir.begin(); di != canonDir.end(); ++di, ++fi) {
    if (fi == canonFile.end() || *fi != *di) return false;
  }
  const auto st = fs::status(canonFile, ec);
  return !ec && st.type() == fs::file_type::regular;
}

std::optional<fs::path> ResourcePack::checkedPath(std::string_view rel) const {
  // Same canonical containment check as has(), but returns the RESOLVED
  // path: opening it removes the has()->open TOCTOU window for the path
  // itself (a swapped symlink now resolves at check time; the returned
  // canonical path names the checked file). See resource_pack.h.
  std::error_code ec;
  if (!isSafePackPath(rel)) return std::nullopt;
  const auto canonFile = fs::canonical(path(rel), ec);
  if (ec) return std::nullopt;
  const auto canonDir = fs::canonical(dir_, ec);
  if (ec) return std::nullopt;
  auto fi = canonFile.begin();
  for (auto di = canonDir.begin(); di != canonDir.end(); ++di, ++fi) {
    if (fi == canonFile.end() || *fi != *di) return std::nullopt;
  }
  const auto st = fs::status(canonFile, ec);
  if (ec || st.type() != fs::file_type::regular) return std::nullopt;
  return canonFile;
}

const ResourcePack* cPackStack::ownerExact(std::string_view rel) const {
  for (auto it = packs_.rbegin(); it != packs_.rend(); ++it)
    if (it->has(rel)) return &*it;
  return nullptr;
}

std::string cPackStack::resolve(std::string_view rel) const {
  if (fallbacks_.empty() || ownerExact(rel)) return std::string(rel);
  for (const auto& [nw, old] : fallbacks_) {
    std::string alt(rel);
    bool hit = false;
    for (size_t at = alt.find(nw); at != std::string::npos; at = alt.find(nw, at + old.size())) {
      alt.replace(at, nw.size(), old);
      hit = true;
    }
    if (hit && ownerExact(alt)) return alt;
  }
  return std::string(rel);
}

const ResourcePack* cPackStack::owner(std::string_view rel) const { return ownerExact(resolve(rel)); }

std::optional<fs::path> cPackStack::find(std::string_view rel0) const {
  const std::string rel = resolve(rel0);
  const ResourcePack* p = owner(rel);
  // Cortana 2026-10-15 1525 (ENGINE, CloudAtlas #4 / CA-04): return the CHECKED (canonical,
  // contained, regular-file) path, so icon loaders no longer reopen the has()->open TOCTOU window.
  return p ? p->checkedPath(rel) : std::nullopt;
}

std::optional<std::string> cPackStack::readText(std::string_view rel0, std::string* origin) const {
  const std::string rel = resolve(rel0);
  const ResourcePack* p = owner(rel);
  if (!p) return std::nullopt;
  if (origin) *origin = p->name();
  // Open the CHECKED (canonical, contained) path, not path(rel): closes
  // the has()->open TOCTOU window (Appendix E L6).
  const auto checked = p->checkedPath(rel);
  if (!checked) return std::nullopt;
  return slurp(*checked);
}

}  // namespace litholattice
