/* lithosys_host_double.c ... HOST TEST DOUBLE implementing the
 * frozen LithoSys calls the stdio / stdlib test links against, in
 * the same spirit as the weave and thread native back-ends: real
 * host services behind the frozen signatures, test scaffolding
 * only, not a production back-end.
 *
 * fd namespace: the LithoSys fd IS the host fd here. 0/1/2 are
 * the process's own descriptors; the volume double returns raw
 * host descriptors >= 3 and they pass straight through. That is
 * the same namespace story the header documents for native
 * back-ends (fd >= 3 is host-defined space).
 *
 * lithosys_memory_grow reports ENOSYS: the tests give WaferHeap a
 * fixed arena and never grow module memory.
 *
 * lithosys_clock_now_ns, lithosys_exit and lithosys_abort are NOT
 * defined here: the weave and thread native back-ends linked into
 * the same test binary already provide them, and a second
 * definition would collide. Their doubles implement the same
 * host services (clock_gettime, exit, abort), so the split is
 * invisible to the code under test. One visible consequence:
 * LithoLibC's time() and clock() run on the thread back-end's
 * clock double in this link. It is a clock_gettime double like
 * this one was.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* <time.h> resolves to LithoLibC's own header in this build, so
 * clock_gettime is declared here directly (struct timespec comes
 * in through <fcntl.h>'s includes) with the host's clock ids. */
int clock_gettime(int clock_id, struct timespec* ts);
#define HOST_CLOCK_REALTIME 0
#define HOST_CLOCK_MONOTONIC 1

#include "lithosys.h"

lithosys_err_t lithosys_memory_grow(uint32_t pages, uint32_t* out_prev) {
  (void)pages;
  (void)out_prev;
  return LITHOSYS_ENOSYS;
}

/* ---- test hooks (audit fix run, 2026-10-28) -------------------------------
 * Scaffolding in the spirit of lithosys_volume_native_set_root:
 * the stdio suite arms these to stage the host behaviors the
 * frozen contract allows and the library must survive. None of
 * this is a back-end surface; the declarations live in the test
 * file that uses them.
 *
 *   lithosys_host_double_arm_eintr_write / _read:
 *     the next fd_write / fd_read returns LITHOSYS_EINTR once,
 *     having moved nothing (finding E-5's regression).
 *   lithosys_host_double_write_script(cap_call, cap, fail_call,
 *                                      zero_call):
 *     scripts the underlying write() calls of fd_write by
 *     1-based call number: cap_call writes at most cap bytes,
 *     fail_call fails with EIO having written nothing, zero_call
 *     returns 0. Zero disables a stage; the script is consumed
 *     (all stages off, counter reset) when the test reads the
 *     call count back through lithosys_host_double_write_calls.
 */
static int g_eintr_write_armed = 0;
static int g_eintr_read_armed = 0;
static int g_write_calls = 0;
static int g_ws_cap_call = 0;
static uint32_t g_ws_cap = 0;
static int g_ws_fail_call = 0;
static int g_ws_zero_call = 0;

void lithosys_host_double_arm_eintr_write(void) { g_eintr_write_armed = 1; }
void lithosys_host_double_arm_eintr_read(void) { g_eintr_read_armed = 1; }

void lithosys_host_double_write_script(int cap_call, uint32_t cap,
                                       int fail_call, int zero_call) {
  g_write_calls = 0;
  g_ws_cap_call = cap_call;
  g_ws_cap = cap;
  g_ws_fail_call = fail_call;
  g_ws_zero_call = zero_call;
}

int lithosys_host_double_write_calls(void) {
  int n = g_write_calls;
  g_ws_cap_call = 0;
  g_ws_fail_call = 0;
  g_ws_zero_call = 0;
  return n;
}

lithosys_err_t lithosys_fd_write(uint32_t fd, const lithosys_iov_t* iovs,
                                 uint32_t iovs_len, uint32_t* out_written) {
  uint32_t total = 0, i;
  if (g_eintr_write_armed) {
    g_eintr_write_armed = 0;
    *out_written = 0;
    return LITHOSYS_EINTR;
  }
  for (i = 0; i < iovs_len; i++) {
    const char* p = (const char*)iovs[i].buf;
    uint32_t left = iovs[i].len;
    while (left > 0) {
      ssize_t n;
      g_write_calls++;
      if (g_write_calls == g_ws_fail_call) {
        n = -1;
        errno = EIO;
      } else if (g_write_calls == g_ws_zero_call) {
        n = 0;
      } else {
        uint32_t ask = left;
        if (g_write_calls == g_ws_cap_call && g_ws_cap < ask) {
          ask = g_ws_cap;
        }
        n = write((int)fd, p, ask);
      }
      /* Audit fix E-2: an error (or a zero return, which the
       * frozen contract cannot express and the old code spun
       * on forever) ends the WHOLE call, honoring the bytes
       * already accepted: success with the true total, never
       * later bytes written after earlier ones failed. The old
       * shape broke only the inner loop on a late error and
       * went on to write the next iov. */
      if (n < 0) {
        if (total > 0) {
          *out_written = total;
          return 0;
        }
        *out_written = 0;
        if (errno == EAGAIN) return LITHOSYS_EAGAIN;
        if (errno == EINTR) return LITHOSYS_EINTR;
        return LITHOSYS_EIO;
      }
      if (n == 0) {
        if (total > 0) {
          *out_written = total;
          return 0;
        }
        *out_written = 0;
        return LITHOSYS_EIO;
      }
      p += n;
      left -= (uint32_t)n;
      total += (uint32_t)n;
    }
  }
  *out_written = total;
  return 0;
}

lithosys_err_t lithosys_fd_read(uint32_t fd, const lithosys_iov_t* iovs,
                                uint32_t iovs_len, uint32_t* out_read) {
  uint32_t total = 0, i;
  if (g_eintr_read_armed) {
    g_eintr_read_armed = 0;
    *out_read = 0;
    return LITHOSYS_EINTR;
  }
  for (i = 0; i < iovs_len; i++) {
    char* p = (char*)iovs[i].buf;
    uint32_t left = iovs[i].len;
    while (left > 0) {
      ssize_t n = read((int)fd, p, left);
      if (n < 0) {
        if (total > 0) break;
        *out_read = 0;
        if (errno == EAGAIN) return LITHOSYS_EAGAIN;
        if (errno == EINTR) return LITHOSYS_EINTR;
        return LITHOSYS_EIO;
      }
      if (n == 0) break; /* EOF: stop filling further iovs too */
      p += n;
      left -= (uint32_t)n;
      total += (uint32_t)n;
    }
    if (left > 0) break; /* short read ends the gather */
  }
  *out_read = total;
  return 0;
}

lithosys_err_t lithosys_fd_close(uint32_t fd) {
  if (fd <= 2) return 0; /* reserved: no-op success, per the freeze */
  return close((int)fd) == 0 ? 0 : LITHOSYS_EBADF;
}

lithosys_err_t lithosys_fd_seek(uint32_t fd, int64_t offset, uint32_t whence,
                                uint64_t* out_pos) {
  int w = (whence == LITHOSYS_SEEK_CUR)
              ? SEEK_CUR
              : (whence == LITHOSYS_SEEK_END) ? SEEK_END : SEEK_SET;
  off_t r = lseek((int)fd, (off_t)offset, w);
  if (r < 0) return (errno == ESPIPE) ? LITHOSYS_ESPIPE : LITHOSYS_EINVAL;
  *out_pos = (uint64_t)r;
  return 0;
}

lithosys_err_t lithosys_fd_sync(uint32_t fd) {
  return fsync((int)fd) == 0 ? 0 : LITHOSYS_EIO;
}

void lithosys_console_write(const char* data, uint32_t len) {
  ssize_t ignored = write(2, data, len);
  (void)ignored;
}

lithosys_err_t lithosys_random_fill(void* buf, uint32_t len) {
  int fd = open("/dev/urandom", O_RDONLY);
  uint32_t done = 0;
  if (fd < 0) return LITHOSYS_EIO;
  while (done < len) {
    ssize_t n = read(fd, (char*)buf + done, len - done);
    if (n <= 0) {
      close(fd);
      return LITHOSYS_EIO;
    }
    done += (uint32_t)n;
  }
  close(fd);
  return 0;
}

uint32_t lithosys_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint32_t)n : 1u;
}
