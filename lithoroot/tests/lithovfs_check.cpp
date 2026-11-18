// lithovfs_check.cpp ... S7 LithoVFS under Node with an in-memory mock
// lk_store (tests/lithovfs_mock.mjs). Returns 0 on pass, else the
// number of the first failing check.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
#define CHECK(n, c) do { if (!(c)) return n; } while (0)

extern "C" int __main_void() {
  std::error_code ec;
  // isatty: 1 only for 0, 1, 2
  CHECK(1, isatty(0) == 1 && isatty(1) == 1 && isatty(2) == 1);
  CHECK(2, isatty(3) == 0 && isatty(64) == 0);
  // empty store: not found, root is a directory
  CHECK(3, fs::status("/saves", ec).type() == fs::file_type::not_found && ec.value() == ENOENT);
  CHECK(4, fs::status("/", ec).type() == fs::file_type::directory && !ec);
  // create_directories, then a file in it through stdio
  CHECK(5, fs::create_directories("saves/world1", ec) && !ec);
  CHECK(6, !fs::create_directories("/saves/world1", ec) && !ec);
  FILE* f = fopen("/saves/world1/level.dat", "wb");
  CHECK(7, f != nullptr);
  int fd = fileno(f);
  CHECK(8, fd >= 64 && isatty(fd) == 0);
  CHECK(9, fwrite("hello lithovfs", 1, 14, f) == 14);
  CHECK(10, fflush(f) == 0 && fsync(fd) == 0);
  CHECK(11, ftruncate(fd, 5) == 0);
  CHECK(12, fclose(f) == 0);
  CHECK(13, fs::file_size("/saves/world1/level.dat", ec) == 5 && !ec);
  // read back
  f = fopen("/saves/world1/level.dat", "rb");
  CHECK(14, f != nullptr);
  char buf[16] = {0};
  CHECK(15, fread(buf, 1, sizeof buf, f) == 5 && std::memcmp(buf, "hello", 5) == 0);
  fclose(f);
  // append
  f = fopen("/saves/world1/level.dat", "ab");
  CHECK(16, f && fwrite("!", 1, 1, f) == 1 && fclose(f) == 0);
  CHECK(17, fs::file_size("/saves/world1/level.dat", ec) == 6);
  // copy, rename, list
  CHECK(18, fs::copy_file("/saves/world1/level.dat", "/saves/world1/level.bak", fs::copy_options::none, ec) && !ec);
  CHECK(19, !fs::copy_file("/saves/world1/level.dat", "/saves/world1/level.bak", fs::copy_options::none, ec) && ec.value() == EEXIST);
  fs::rename("/saves/world1", "/saves/world2", ec);
  CHECK(20, !ec && fs::status("/saves/world2/level.bak", ec).type() == fs::file_type::regular);
  CHECK(21, fs::status("/saves/world1", ec).type() == fs::file_type::not_found);
  std::vector<std::string> names;
  CHECK(22, fs::__lk_list_dir("/saves/world2", names, ec) && names.size() == 2);
  // resize, times, canonical
  fs::resize_file("/saves/world2/level.bak", 100, ec);
  CHECK(23, !ec && fs::file_size("/saves/world2/level.bak", ec) == 100);
  (void)fs::last_write_time("/saves/world2/level.bak", ec);
  CHECK(24, !ec);
  CHECK(25, fs::canonical("/saves/./world2/../world2/level.dat", ec) == fs::path("/saves/world2/level.dat") && !ec);
  CHECK(26, fs::canonical("/saves/nope", ec).empty() && ec.value() == ENOENT);
  CHECK(27, fs::weakly_canonical("a/b/../c", ec) == fs::path("/a/c") && !ec);
  CHECK(28, fs::current_path(ec) == fs::path("/"));
  // fail-safe: symlinks, temp dir, read-only miss
  fs::create_symlink("/saves", "/link", ec);
  CHECK(29, ec.value() == ENOTSUP);
  CHECK(30, fs::symlink_status("/saves", ec).type() == fs::file_type::directory);
  CHECK(31, fs::temp_directory_path(ec) == fs::path("/tmp") && fs::status("/tmp", ec).type() == fs::file_type::directory);
  CHECK(32, fopen("/nothing/here", "rb") == nullptr && errno == ENOENT);
  CHECK(33, fopen("/nothing/here", "wb") == nullptr && errno == ENOENT); // parent missing
  CHECK(34, fsync(99) == -1 && errno == EBADF);
  // the pack (mock: /pack/readme.txt = "from the pack"): readable,
  // modify-in-place is EROFS, a truncating open shadows it
  f = fopen("/pack/readme.txt", "rb");
  CHECK(38, f != nullptr);
  CHECK(39, fread(buf, 1, 13, f) == 13 && std::memcmp(buf, "from the pack", 13) == 0);
  fclose(f);
  CHECK(40, fs::file_size("/pack/readme.txt", ec) == 13 && !ec);
  errno = 0;
  CHECK(41, fopen("/pack/readme.txt", "r+b") == nullptr && errno == EROFS);
  errno = 0;
  CHECK(42, fopen("/pack/readme.txt", "ab") == nullptr && errno == EROFS);
  CHECK(43, !fs::remove("/pack/readme.txt", ec) && ec.value() == EROFS);
  fs::create_directories("/pack", ec);
  f = fopen("/pack/readme.txt", "wb");
  CHECK(44, f && fwrite("mine", 1, 4, f) == 4 && fclose(f) == 0);
  CHECK(45, fs::file_size("/pack/readme.txt", ec) == 4); // store shadows pack
  CHECK(46, fs::remove_all("/pack", ec) == 2 && !ec);
  CHECK(47, fs::file_size("/pack/readme.txt", ec) == 13); // pack visible again
  // remove_all counts files and directories
  CHECK(35, fs::remove_all("/saves", ec) == 4 && !ec);
  CHECK(36, fs::status("/saves", ec).type() == fs::file_type::not_found);
  CHECK(37, !fs::remove("/saves", ec) && !ec);
  return 0;
}
