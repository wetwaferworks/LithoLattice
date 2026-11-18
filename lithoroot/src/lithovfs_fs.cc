// lithovfs_fs.cc ... S7: the std::filesystem operations LithoKraft uses,
// over LithoVFS (src/libc/lithovfs.c). Web only. Symlinks fail safe
// with ENOTSUP; temp_directory_path is /tmp inside the overlay;
// current_path is "/" (the overlay root) and cannot be changed.
#include <filesystem>
#include <string>
#include <vector>

#include <cerrno>
#include <cstdlib>

#include "libc/lithovfs.h"

namespace std {
namespace filesystem {
namespace {
inline void set(error_code& ec, int neg) { ec = error_code(-neg, generic_category()); }
inline string norm(const path& p) {
  char buf[LV_PATH_MAX];
  int r = lv_normalize(p.c_str(), buf, sizeof buf);
  return r < 0 ? string() : string(buf);
}
}  // namespace

file_status status(const path& p, error_code& ec) noexcept {
  lv_stat_t st;
  int r = lv_stat(p.c_str(), &st);
  if (r < 0) {
    set(ec, r);
    return file_status(r == -ENOENT ? file_type::not_found : file_type::none);
  }
  ec.clear();
  return file_status(st.type == LV_DIR ? file_type::directory : file_type::regular,
                     st.in_pack ? perms(0444) : perms(0644));
}

// No symlinks exist in LithoVFS, so the link's status is the status.
file_status symlink_status(const path& p, error_code& ec) noexcept { return status(p, ec); }

bool create_directory(const path& p, error_code& ec) noexcept {
  int r = lv_mkdir(p.c_str());
  if (r == -EEXIST) {
    lv_stat_t st;
    if (lv_stat(p.c_str(), &st) == 0 && st.type == LV_DIR) { ec.clear(); return false; }
  }
  if (r < 0) { set(ec, r); return false; }
  ec.clear();
  return true;
}

bool create_directories(const path& p, error_code& ec) {
  string n = norm(p);
  bool made = false;
  if (n.empty()) { set(ec, -EINVAL); return false; }
  for (size_t i = 1; i <= n.size(); ++i) {
    if (i != n.size() && n[i] != '/') continue;
    string part = n.substr(0, i);
    lv_stat_t st;
    if (lv_stat(part.c_str(), &st) == 0) {
      if (st.type != LV_DIR) { set(ec, -ENOTDIR); return false; }
      continue;
    }
    if (!create_directory(path(part), ec)) return false;
    made = true;
  }
  ec.clear();
  return made;
}

void create_symlink(const path&, const path&, error_code& ec) noexcept { set(ec, -ENOTSUP); }

bool copy_file(const path& from, const path& to, copy_options o, error_code& ec) {
  lv_stat_t st;
  if (lv_stat(to.c_str(), &st) == 0) {
    if ((o & copy_options::overwrite_existing) == copy_options::none) {
      if ((o & copy_options::skip_existing) != copy_options::none) { ec.clear(); return false; }
      set(ec, -EEXIST);
      return false;
    }
  }
  int in = lv_open(from.c_str(), 0 /* O_RDONLY */);
  if (in < 0) { set(ec, in); return false; }
  int out = lv_open(to.c_str(), 01 | 0100 | 01000 /* O_WRONLY|O_CREAT|O_TRUNC */);
  if (out < 0) { lv_close(in); set(ec, out); return false; }
  char buf[4096];
  for (;;) {
    long long n = lv_read(in, buf, sizeof buf);
    if (n < 0) { lv_close(in); lv_close(out); set(ec, (int)n); return false; }
    if (n == 0) break;
    long long w = lv_write(out, buf, (size_t)n);
    if (w != n) { lv_close(in); lv_close(out); set(ec, w < 0 ? (int)w : -EIO); return false; }
  }
  lv_close(in);
  int r = lv_close(out);
  if (r < 0) { set(ec, r); return false; }
  ec.clear();
  return true;
}

uintmax_t file_size(const path& p, error_code& ec) noexcept {
  lv_stat_t st;
  int r = lv_stat(p.c_str(), &st);
  if (r < 0) { set(ec, r); return static_cast<uintmax_t>(-1); }
  if (st.type == LV_DIR) { set(ec, -EISDIR); return static_cast<uintmax_t>(-1); }
  ec.clear();
  return st.size;
}

file_time_type last_write_time(const path& p, error_code& ec) noexcept {
  lv_stat_t st;
  int r = lv_stat(p.c_str(), &st);
  if (r < 0) { set(ec, r); return file_time_type::min(); }
  ec.clear();
  return file_time_type(chrono::nanoseconds(st.mtime_ns));
}

bool remove(const path& p, error_code& ec) noexcept {
  int r = lv_remove(p.c_str());
  if (r == -ENOENT) { ec.clear(); return false; }
  if (r < 0) { set(ec, r); return false; }
  ec.clear();
  return true;
}

uintmax_t remove_all(const path& p, error_code& ec) {
  lv_stat_t st;
  int r = lv_stat(p.c_str(), &st);
  if (r == -ENOENT) { ec.clear(); return 0; }
  if (r < 0) { set(ec, r); return static_cast<uintmax_t>(-1); }
  uintmax_t n = 0;
  if (st.type == LV_DIR) {
    char* names = nullptr;
    int c = lv_list(p.c_str(), &names);
    if (c < 0) { set(ec, c); return static_cast<uintmax_t>(-1); }
    const char* q = names;
    string base = norm(p);
    for (int i = 0; i < c; ++i, q += __builtin_strlen(q) + 1) {
      uintmax_t k = remove_all(path(base + "/" + q), ec);
      if (ec) { free(names); return static_cast<uintmax_t>(-1); }
      n += k;
    }
    free(names);
  }
  if (!remove(p, ec) && ec) return static_cast<uintmax_t>(-1);
  return n + 1;
}

void rename(const path& from, const path& to, error_code& ec) noexcept {
  int r = lv_rename(from.c_str(), to.c_str());
  if (r < 0) set(ec, r); else ec.clear();
}

void resize_file(const path& p, uintmax_t n, error_code& ec) noexcept {
  int r = lv_truncate_path(p.c_str(), n);
  if (r < 0) set(ec, r); else ec.clear();
}

path temp_directory_path(error_code& ec) {
  lv_stat_t st;
  if (lv_stat("/tmp", &st) != 0) {
    int r = lv_mkdir("/tmp");
    if (r < 0 && r != -EEXIST) { set(ec, r); return path(); }
  }
  ec.clear();
  return path("/tmp");
}

path current_path(error_code& ec) { ec.clear(); return path("/"); }

path canonical(const path& p, error_code& ec) {
  string n = norm(p);
  lv_stat_t st;
  if (n.empty()) { set(ec, -EINVAL); return path(); }
  int r = lv_stat(n.c_str(), &st);
  if (r < 0) { set(ec, r); return path(); }
  ec.clear();
  return path(n);
}

// No symlinks: lexical normalization against "/" is exact.
path weakly_canonical(const path& p, error_code& ec) {
  string n = norm(p);
  if (n.empty()) { set(ec, -EINVAL); return path(); }
  ec.clear();
  return path(n);
}

bool __lk_list_dir(const path& p, vector<string>& names, error_code& ec) {
  char* buf = nullptr;
  int c = lv_list(p.c_str(), &buf);
  names.clear();
  if (c < 0) { set(ec, c); return false; }
  const char* q = buf;
  for (int i = 0; i < c; ++i, q += __builtin_strlen(q) + 1) names.emplace_back(q);
  free(buf);
  ec.clear();
  return true;
}
}  // namespace filesystem
}  // namespace std
