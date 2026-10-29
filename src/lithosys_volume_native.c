/* lithosys_volume_native.c ... HOST TEST DOUBLE for
 * lithosys_volume_open (declared in lithosys.h, ruling addendum
 * in docs/LITHOSYS-FREEZE.md).
 *
 * This is test scaffolding in the same pattern as the weave and
 * thread native back-ends: it serves a plain host directory as
 * the read-only volume, so the LithoLibC stdio layer can be
 * tested on a development machine. It is NOT the production
 * implementation of anything. On the web host, lkboot registers
 * the fetched pack blob and serves the same call out of it; see
 * docs/LITHOLIBC-STDIO.md for that integration's open shape.
 *
 * The root directory is set once by
 * lithosys_volume_native_set_root(). Paths are joined under the
 * root with traversal rejected: absolute paths and any ".." path
 * component fail with LITHOSYS_EPERM (audit doc fix E-9: this
 * header used to name LITHOSYS_EACCES, which does not exist in
 * the frozen set; EPERM, "host refused on policy", is what the
 * code has always returned), because a volume path is a
 * pack-relative name by definition. Returned fds are host file
 * descriptors, moved to 3 or above before they are handed out
 * (see the minting checks in lithosys_volume_open and the host
 * minting note in docs/LITHOSYS-FREEZE.md); the host fd double
 * in tests/ passes them through to the frozen fd calls.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lithosys.h"

#define VOLUME_ROOT_MAX 1024

static char g_volume_root[VOLUME_ROOT_MAX];
static int g_volume_root_set = 0;

void lithosys_volume_native_set_root(const char* root) {
  size_t n = strlen(root);
  if (n == 0 || n >= VOLUME_ROOT_MAX) {
    g_volume_root_set = 0;
    return;
  }
  memcpy(g_volume_root, root, n + 1);
  /* Trim one trailing slash so the join below has exactly one. */
  if (n > 1 && g_volume_root[n - 1] == '/') g_volume_root[n - 1] = '\0';
  g_volume_root_set = 1;
}

lithosys_err_t lithosys_volume_open(const char* path, uint32_t* out_fd) {
  char joined[VOLUME_ROOT_MAX + 1024];
  size_t rl, pl, i;
  int fd;

  if (!g_volume_root_set) return LITHOSYS_ENOSYS;
  if (path == NULL || out_fd == NULL) return LITHOSYS_EINVAL;
  if (path[0] == '/') return LITHOSYS_EPERM;
  /* Reject any ".." component (start, middle or end). */
  for (i = 0; path[i] != '\0'; i++) {
    if (path[i] == '.' && path[i + 1] == '.' &&
        (i == 0 || path[i - 1] == '/') &&
        (path[i + 2] == '/' || path[i + 2] == '\0')) {
      return LITHOSYS_EPERM;
    }
  }

  rl = strlen(g_volume_root);
  pl = strlen(path);
  if (rl + 1 + pl + 1 > sizeof joined) return LITHOSYS_EINVAL;
  memcpy(joined, g_volume_root, rl);
  joined[rl] = '/';
  memcpy(joined + rl + 1, path, pl + 1);

  fd = open(joined, O_RDONLY);
  if (fd < 0) {
    /* Host errnos map into the frozen set deliberately: the
     * frozen enum is a sparse subset of the WASI numbering, so
     * EACCES becomes EPERM (host refused on policy) and anything
     * else unexpected reads as a plain miss (ENOENT), which is
     * how fopen is specified to treat a volume lookup failure. */
    switch (errno) {
      case EACCES:
        return LITHOSYS_EPERM;
      case ENAMETOOLONG:
        return LITHOSYS_EINVAL;
      default:
        return LITHOSYS_ENOENT;
    }
  }
  /* Audit fix E-3: shape-check what was actually opened before
   * minting it as a volume fd. open() alone follows symlinks
   * wherever they point (the freeze doc now names symlink
   * policy a host obligation; this double serves a plain
   * directory and follows them), and it opens directories
   * without complaint: a volume serves regular pack files, and
   * anything else is a miss (fopen's volume contract fails soft
   * with ENOENT). And when a standard descriptor stands closed,
   * open() returns its number: a minted fd lives in the
   * volume's own namespace at 3 or above, so the descriptor is
   * moved up before it is handed out, rather than aliasing the
   * process's stdin, stdout or stderr. */
  {
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      close(fd);
      return LITHOSYS_ENOENT;
    }
  }
  if (fd < 3) {
    int moved = fcntl(fd, F_DUPFD, 3);
    if (moved < 0) {
      close(fd);
      return LITHOSYS_EIO;
    }
    close(fd);
    fd = moved;
  }
  *out_fd = (uint32_t)fd;
  return 0;
}
