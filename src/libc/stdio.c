/* stdio.c ... the LithoLibC FILE layer over the frozen LithoSys
 * fd calls and the read-only volume.
 *
 * Layering (the note in docs/LITHOLIBC-STDIO.md carries the full
 * story): ordinary output goes FILE buffer -> lithosys_fd_write;
 * lithosys_console_write is NEVER used by the normal path. It is
 * the sink of last resort, owned by the assert and abort
 * reporters (LithoAbi's diagnostics and stdlib's abort flush
 * fallback), which is what its freeze comment specifies: libc
 * state may be what just broke. stdio keeps exactly one emergency
 * behavior of its own: nothing. If fd_write fails, the stream's
 * error flag is set and the caller hears about it; stdio does not
 * silently reroute user data to the console sink.
 *
 * The fopen routing (ruling addendum, docs/LITHOSYS-FREEZE.md):
 * every path is looked up in the read-only volume FIRST through
 * lithosys_volume_open. Read modes against a hit open the volume
 * fd. Write modes against a hit fail EROFS. Any write mode
 * against a miss fails ENOSYS: the frozen surface has no writable
 * filesystem, and persistence (saves, logs on device) rides the
 * lk platform layer, not stdio. remove and rename fail ENOSYS
 * for the same reason. This fail-soft table is deliberate and
 * documented per function in the note.
 *
 * Buffering: stdin fully buffered, stdout line buffered, stderr
 * unbuffered (the C defaults for a terminal-less host). One
 * global LithoThread mutex serializes stream state; per-stream
 * locking beyond that is the caller's business, as in the
 * classic contract.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "litholibc_internal.h"
#include "lithosys.h"

struct litholibc_file {
  struct litholibc_file* next; /* open-stream registry */
  int fd;                      /* LithoSys fd, -1 when closed */
  int is_volume;               /* fd came from the volume */
  int readable, writable;
  int error_flag, eof_flag;
  int bufmode; /* _IOFBF, _IOLBF, _IONBF */
  int dir;     /* 0 fresh, 1 reading, 2 writing */
  unsigned char* buf;
  size_t buf_size;
  size_t buf_len; /* read: bytes held; write: bytes pending */
  size_t buf_pos; /* read cursor */
  int buf_owned;
  long long fd_pos; /* fd offset just past the last raw transfer */
  unsigned char ungot[8];
  int ungot_count;
};

/* ---- the plumbing lock ------------------------------------------------ */

static pthread_mutex_t g_stdio_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_stdio_lock)
#define UNLOCK() pthread_mutex_unlock(&g_stdio_lock)

static FILE* g_registry = NULL;

static void registry_add(FILE* f) {
  f->next = g_registry;
  g_registry = f;
}
static void registry_remove(FILE* f) {
  FILE** pp = &g_registry;
  while (*pp) {
    if (*pp == f) {
      *pp = f->next;
      return;
    }
    pp = &(*pp)->next;
  }
}

/* ---- the standard streams ----------------------------------------------- */

static unsigned char g_stdin_buf[1024];
static unsigned char g_stdout_buf[1024];

static struct litholibc_file g_stdin_file = {
    .fd = 0, .readable = 1, .bufmode = _IOFBF, .buf = g_stdin_buf,
    .buf_size = sizeof g_stdin_buf, .fd_pos = 0};
static struct litholibc_file g_stdout_file = {
    .fd = 1, .writable = 1, .bufmode = _IOLBF, .buf = g_stdout_buf,
    .buf_size = sizeof g_stdout_buf, .fd_pos = 0};
static struct litholibc_file g_stderr_file = {
    .fd = 2, .writable = 1, .bufmode = _IONBF, .fd_pos = 0};

FILE* stdin = &g_stdin_file;
FILE* stdout = &g_stdout_file;
FILE* stderr = &g_stderr_file;

static int g_registry_ready = 0;
static void registry_init_locked(void) {
  if (g_registry_ready) return;
  g_registry_ready = 1;
  registry_add(&g_stdin_file);
  registry_add(&g_stdout_file);
  registry_add(&g_stderr_file);
}

/* ---- raw transfers -------------------------------------------------------- */

/* Single-iov transfers with the EINTR retry (audit fix E-5): a
 * host may interrupt a transfer before it moves anything, and
 * LITHOSYS_EINTR is a frozen, producible code. Retrying here
 * keeps an interruption from permanently erroring a stream over
 * a transfer that never happened; every fd_read / fd_write call
 * in this file goes through one of these two helpers. */
static lithosys_err_t fd_write_1(int fd, const lithosys_iov_t* iov,
                                 uint32_t* out_written) {
  lithosys_err_t err;
  do {
    err = lithosys_fd_write((uint32_t)fd, iov, 1, out_written);
  } while (err == LITHOSYS_EINTR);
  return err;
}

static lithosys_err_t fd_read_1(int fd, lithosys_iov_t* iov,
                                uint32_t* out_read) {
  lithosys_err_t err;
  do {
    err = lithosys_fd_read((uint32_t)fd, iov, 1, out_read);
  } while (err == LITHOSYS_EINTR);
  return err;
}

/* Flush the first n pending write bytes, keeping the rest.
 * Lock held. Returns 0 or -1 (the unflushed tail is kept). */
static int flush_n_locked(FILE* f, size_t n) {
  size_t done = 0;
  while (done < n) {
    lithosys_iov_t iov;
    uint32_t wrote = 0;
    lithosys_err_t err;
    iov.buf = f->buf + done;
    iov.len = (uint32_t)(n - done);
    err = fd_write_1(f->fd, &iov, &wrote);
    if (err != LITHOSYS_OK || wrote == 0) {
      f->error_flag = 1;
      memmove(f->buf, f->buf + done, f->buf_len - done);
      f->buf_len -= done;
      errno = (err == LITHOSYS_OK) ? EIO : (int)err;
      return -1;
    }
    done += wrote;
    f->fd_pos += wrote;
  }
  memmove(f->buf, f->buf + n, f->buf_len - n);
  f->buf_len -= n;
  return 0;
}

/* Flush all pending write bytes. Lock held. */
static int flush_locked(FILE* f) {
  int rc;
  if (f->dir != 2 || f->buf_len == 0) {
    if (f->dir == 2) f->dir = 0;
    return 0;
  }
  rc = flush_n_locked(f, f->buf_len);
  if (rc == 0) f->dir = 0;
  return rc;
}

/* Refill the read buffer. Lock held. Returns bytes available
 * (possibly 0 at EOF with eof_flag set) or -1 on error. */
static long fill_locked(FILE* f) {
  lithosys_iov_t iov;
  uint32_t got = 0;
  lithosys_err_t err;
  if (f->bufmode == _IONBF || f->buf == NULL || f->buf_size == 0) {
    /* Ungetc space only: reads go one byte at a time through a
     * stack slot by the callers in this case. */
    return 0;
  }
  iov.buf = f->buf;
  iov.len = (uint32_t)f->buf_size;
  err = fd_read_1(f->fd, &iov, &got);
  if (err != LITHOSYS_OK) {
    f->error_flag = 1;
    errno = (int)err;
    return -1;
  }
  f->buf_len = got;
  f->buf_pos = 0;
  f->fd_pos += got;
  if (got == 0) f->eof_flag = 1;
  return (long)got;
}

/* ---- internal write path (printf's sink, puts) --------------------------- */

int __litholibc_file_write(FILE* f, const char* data, size_t len) {
  int rc = 0;
  LOCK();
  if (f->fd < 0 || !f->writable) {
    errno = EBADF;
    UNLOCK();
    return -1;
  }
  if (f->dir == 1) {
    /* Write after read: drop the unread buffer (the fd position
     * stays where the raw reads left it, the usual behavior). */
    f->buf_len = 0;
    f->buf_pos = 0;
    f->ungot_count = 0;
    f->dir = 0;
  }
  f->dir = 2;
  if (f->bufmode == _IONBF || f->buf == NULL) {
    size_t done = 0;
    while (done < len) {
      lithosys_iov_t iov;
      uint32_t wrote = 0;
      lithosys_err_t err;
      iov.buf = data + done;
      iov.len = (uint32_t)(len - done);
      err = fd_write_1(f->fd, &iov, &wrote);
      if (err != LITHOSYS_OK || wrote == 0) {
        f->error_flag = 1;
        errno = (err == LITHOSYS_OK) ? EIO : (int)err;
        rc = -1;
        break;
      }
      done += wrote;
      f->fd_pos += wrote;
    }
    UNLOCK();
    return rc;
  }
  /* Buffered: copy through the buffer, flushing as it fills. A
   * write larger than the buffer flushes what is pending and
   * then goes direct for the bulk. */
  {
    size_t off = 0;
    if (len >= f->buf_size) {
      if (flush_locked(f) != 0) rc = -1;
      f->dir = 2;
      while (off < len && rc == 0) {
        lithosys_iov_t iov;
        uint32_t wrote = 0;
        lithosys_err_t err;
        iov.buf = data + off;
        iov.len = (uint32_t)(len - off);
        err = fd_write_1(f->fd, &iov, &wrote);
        if (err != LITHOSYS_OK || wrote == 0) {
          f->error_flag = 1;
          errno = (err == LITHOSYS_OK) ? EIO : (int)err;
          rc = -1;
          break;
        }
        off += wrote;
        f->fd_pos += wrote;
      }
    }
    while (off < len && rc == 0) {
      size_t room = f->buf_size - f->buf_len;
      size_t take = (len - off) < room ? (len - off) : room;
      memcpy(f->buf + f->buf_len, data + off, take);
      f->buf_len += take;
      off += take;
      if (f->buf_len == f->buf_size) {
        if (flush_locked(f) != 0) rc = -1;
        f->dir = 2;
      }
    }
    /* Line buffered: flush through the last newline present. */
    if (rc == 0 && f->bufmode == _IOLBF && f->buf_len > 0) {
      size_t i;
      size_t cut = 0;
      for (i = 0; i < f->buf_len; i++) {
        if (f->buf[i] == '\n') cut = i + 1;
      }
      if (cut > 0) {
        if (flush_n_locked(f, cut) != 0) rc = -1;
        f->dir = 2; /* still in write mode, tail pending */
      }
    }
  }
  UNLOCK();
  return rc;
}

/* ---- fopen / fclose ------------------------------------------------------- */

FILE* fopen(const char* path, const char* mode) {
  int readable = 0, writable = 0, append = 0;
  const char* m = mode;
  uint32_t vfd = 0;
  lithosys_err_t verr;
  FILE* f;

  if (path == NULL || mode == NULL) {
    errno = EINVAL;
    return NULL;
  }
  switch (*m) {
    case 'r': readable = 1; break;
    case 'w': writable = 1; break;
    case 'a': writable = 1; append = 1; break;
    default:
      errno = EINVAL;
      return NULL;
  }
  for (m++; *m; m++) {
    if (*m == '+') {
      readable = 1;
      writable = 1;
    }
    /* 'b' and 'x' and 'e' and friends: accepted and inert (binary
     * is the only mode a wasm host has; exclusivity is moot
     * against a read-only volume and an absent writable fs). */
  }
  (void)append; /* no seekable writable target exists yet */
  (void)readable; /* routing keys on writable alone: a hit with
                     any write intent is EROFS, a plain read opens */

  /* Routing: volume first, always. */
  verr = lithosys_volume_open(path, &vfd);
  if (verr == LITHOSYS_OK) {
    if (writable) {
      (void)lithosys_fd_close(vfd);
      errno = EROFS;
      return NULL;
    }
    f = (FILE*)malloc(sizeof *f);
    if (f == NULL) {
      (void)lithosys_fd_close(vfd);
      errno = ENOMEM;
      return NULL;
    }
    memset(f, 0, sizeof *f);
    f->fd = (int)vfd;
    f->is_volume = 1;
    f->readable = 1;
    f->writable = 0;
    f->bufmode = _IOFBF;
    f->buf_size = BUFSIZ;
    f->buf = (unsigned char*)malloc(BUFSIZ);
    f->buf_owned = 1;
    if (f->buf == NULL) {
      (void)lithosys_fd_close(vfd);
      free(f);
      errno = ENOMEM;
      return NULL;
    }
    LOCK();
    registry_init_locked();
    registry_add(f);
    UNLOCK();
    return f;
  }
  /* A miss. Read modes report the miss; write modes have nowhere
   * to go in the frozen surface at all. */
  if (writable) {
    errno = ENOSYS;
    return NULL;
  }
  /* Audit fix E-4: the volume's own code passes through, the
   * zero-translation rule every other path in this file honors.
   * The volume contract produces only codes that are valid
   * errnos by construction (a miss is ENOENT, a policy refusal
   * EPERM, a host failure its own code); collapsing them all
   * into ENOENT misreported each of them. */
  errno = (int)verr;
  return NULL;
}

int fclose(FILE* f) {
  int rc = 0;
  if (f == NULL) {
    errno = EINVAL;
    return EOF;
  }
  LOCK();
  if (f->dir == 2) {
    if (flush_locked(f) != 0) rc = EOF;
  }
  if (f->fd >= 0) {
    if (lithosys_fd_close((uint32_t)f->fd) != LITHOSYS_OK) rc = EOF;
    f->fd = -1;
  }
  registry_remove(f);
  UNLOCK();
  if (f != &g_stdin_file && f != &g_stdout_file && f != &g_stderr_file) {
    if (f->buf_owned) free(f->buf);
    free(f);
  }
  return rc;
}

int fflush(FILE* f) {
  int rc = 0;
  LOCK();
  registry_init_locked();
  if (f == NULL) {
    FILE* it;
    for (it = g_registry; it != NULL; it = it->next) {
      if (it->dir == 2 && flush_locked(it) != 0) rc = EOF;
    }
  } else if (f->dir == 2) {
    if (flush_locked(f) != 0) rc = EOF;
  }
  UNLOCK();
  return rc;
}

void __litholibc_stdio_flush_all(void) { (void)fflush(NULL); }

/* ---- input ------------------------------------------------------------------ */

/* Read one byte with the lock held, through ungetc and the
 * buffer. Returns the byte or EOF. */
static int getc_locked(FILE* f) {
  if (f->ungot_count > 0) {
    f->eof_flag = 0;
    return f->ungot[--f->ungot_count];
  }
  if (f->dir == 2) {
    if (flush_locked(f) != 0) return EOF;
  }
  f->dir = 1;
  if (f->buf_pos < f->buf_len) {
    return f->buf[f->buf_pos++];
  }
  if (f->bufmode == _IONBF || f->buf == NULL || f->buf_size == 0) {
    /* Unbuffered: one byte straight from the fd. */
    unsigned char c;
    lithosys_iov_t iov;
    uint32_t got = 0;
    lithosys_err_t err;
    iov.buf = &c;
    iov.len = 1;
    err = fd_read_1(f->fd, &iov, &got);
    if (err != LITHOSYS_OK) {
      f->error_flag = 1;
      errno = (int)err;
      return EOF;
    }
    if (got == 0) {
      f->eof_flag = 1;
      return EOF;
    }
    f->fd_pos += 1;
    return c;
  }
  if (fill_locked(f) <= 0) return EOF;
  return f->buf[f->buf_pos++];
}

int fgetc(FILE* f) {
  int c;
  LOCK();
  if (f->fd < 0 || !f->readable) {
    errno = EBADF;
    UNLOCK();
    return EOF;
  }
  c = getc_locked(f);
  UNLOCK();
  return c;
}
int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE* f) {
  LOCK();
  if (c == EOF || f->fd < 0 || !f->readable) {
    UNLOCK();
    return EOF;
  }
  if (f->dir == 1 && f->buf_pos > 0 && f->buf != NULL) {
    /* Reuse the just-consumed slot (the classic trick): the
     * pushed byte replaces whatever was there, which is exactly
     * what the next read must see. */
    f->buf[--f->buf_pos] = (unsigned char)c;
    f->eof_flag = 0;
    UNLOCK();
    return c;
  }
  if (f->ungot_count < (int)sizeof f->ungot) {
    f->ungot[f->ungot_count++] = (unsigned char)c;
    f->eof_flag = 0;
    if (f->dir == 0) f->dir = 1;
    UNLOCK();
    return c;
  }
  UNLOCK();
  return EOF;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
  size_t total, done = 0;
  unsigned char* out = (unsigned char*)ptr;
  if (size == 0 || nmemb == 0) return 0;
  /* Audit fix B-7 / E-6: the product must not wrap (on wasm32 a
   * 4 GiB total wraps to a small count and the call silently
   * transfers the wrapped amount). Refuse up front, the same
   * guard wh_calloc puts on its product. */
  if (nmemb > ((size_t)-1) / size) {
    errno = ENOMEM;
    return 0;
  }
  total = size * nmemb;
  LOCK();
  if (f->fd < 0 || !f->readable) {
    errno = EBADF;
    UNLOCK();
    return 0;
  }
  /* Ungetc bytes first. */
  while (done < total && f->ungot_count > 0) {
    out[done++] = f->ungot[--f->ungot_count];
    f->eof_flag = 0;
  }
  if (f->dir == 2) {
    if (flush_locked(f) != 0) {
      UNLOCK();
      return done / size;
    }
  }
  f->dir = 1;
  /* Buffered content next. */
  while (done < total && f->buf_pos < f->buf_len) {
    out[done++] = f->buf[f->buf_pos++];
  }
  /* Bulk: read directly into the caller's buffer for the rest
   * when the remainder is at least a bufferful; otherwise refill
   * and serve from the buffer. */
  while (done < total) {
    size_t remain = total - done;
    if (remain >= f->buf_size && f->buf_size > 0) {
      lithosys_iov_t iov;
      uint32_t got = 0;
      lithosys_err_t err;
      iov.buf = out + done;
      iov.len = (uint32_t)remain;
      err = fd_read_1(f->fd, &iov, &got);
      if (err != LITHOSYS_OK) {
        f->error_flag = 1;
        errno = (int)err;
        break;
      }
      if (got == 0) {
        f->eof_flag = 1;
        break;
      }
      f->fd_pos += got;
      done += got;
    } else if (f->bufmode == _IONBF || f->buf == NULL ||
               f->buf_size == 0) {
      /* Unbuffered or bufferless (audit fix B-6): fill_locked
       * returns 0 by design for these streams, which this loop
       * would read as EOF and report a live stream as empty.
       * Read the remainder straight from the fd, the bulk form
       * of getc_locked's one-byte direct path. */
      lithosys_iov_t iov;
      uint32_t got = 0;
      lithosys_err_t err;
      iov.buf = out + done;
      iov.len = (uint32_t)remain;
      err = fd_read_1(f->fd, &iov, &got);
      if (err != LITHOSYS_OK) {
        f->error_flag = 1;
        errno = (int)err;
        break;
      }
      if (got == 0) {
        f->eof_flag = 1;
        break;
      }
      f->fd_pos += got;
      done += got;
    } else {
      long filled = fill_locked(f);
      if (filled <= 0) break;
      while (done < total && f->buf_pos < f->buf_len) {
        out[done++] = f->buf[f->buf_pos++];
      }
    }
  }
  UNLOCK();
  return done / size;
}

char* fgets(char* s, int n, FILE* f) {
  int i = 0;
  if (n <= 0) return NULL;
  LOCK();
  if (f->fd < 0 || !f->readable) {
    errno = EBADF;
    UNLOCK();
    return NULL;
  }
  while (i < n - 1) {
    int c = getc_locked(f);
    if (c == EOF) break;
    s[i++] = (char)c;
    if (c == '\n') break;
  }
  UNLOCK();
  if (i == 0) return NULL;
  s[i] = '\0';
  return s;
}

/* ---- output ------------------------------------------------------------------- */

int fputc(int c, FILE* f) {
  unsigned char uc = (unsigned char)c;
  if (__litholibc_file_write(f, (const char*)&uc, 1) != 0) return EOF;
  return uc;
}
int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char* s, FILE* f) {
  return __litholibc_file_write(f, s, strlen(s)) == 0 ? 0 : EOF;
}

int puts(const char* s) {
  if (__litholibc_file_write(stdout, s, strlen(s)) != 0) return EOF;
  if (__litholibc_file_write(stdout, "\n", 1) != 0) return EOF;
  return 0;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
  if (size == 0 || nmemb == 0) return 0;
  /* Audit fix B-7 / E-6: refuse a wrapping product up front
   * (see fread): the wrapped call transferred a few bytes and
   * then reported every element written. */
  if (nmemb > ((size_t)-1) / size) {
    LOCK();
    f->error_flag = 1;
    UNLOCK();
    errno = ENOMEM;
    return 0;
  }
  if (__litholibc_file_write(f, (const char*)ptr, size * nmemb) != 0) {
    return 0;
  }
  return nmemb;
}

/* ---- positioning ---------------------------------------------------------------- */

int fseek(FILE* f, long offset, int whence) {
  uint64_t pos = 0;
  lithosys_err_t err;
  LOCK();
  if (f->fd < 0) {
    errno = EBADF;
    UNLOCK();
    return -1;
  }
  if (f->dir == 2) {
    if (flush_locked(f) != 0) {
      UNLOCK();
      return -1;
    }
  }
  /* Adjust for unread buffered input on SEEK_CUR: the logical
   * position trails the fd's raw position. */
  if (whence == SEEK_CUR && f->dir == 1) {
    long unread = (long)(f->buf_len - f->buf_pos) + f->ungot_count;
    offset -= unread;
  }
  err = lithosys_fd_seek((uint32_t)f->fd, (int64_t)offset, (uint32_t)whence,
                         &pos);
  if (err != LITHOSYS_OK) {
    errno = (int)err;
    UNLOCK();
    return -1;
  }
  f->fd_pos = (long long)pos;
  f->buf_len = 0;
  f->buf_pos = 0;
  f->ungot_count = 0;
  f->eof_flag = 0;
  f->dir = 0;
  UNLOCK();
  return 0;
}

long ftell(FILE* f) {
  long long pos;
  LOCK();
  if (f->fd < 0) {
    errno = EBADF;
    UNLOCK();
    return -1;
  }
  pos = f->fd_pos;
  if (f->dir == 1) {
    pos -= (long long)(f->buf_len - f->buf_pos) + f->ungot_count;
  } else if (f->dir == 2) {
    pos += (long long)f->buf_len;
  }
  UNLOCK();
  return (long)pos;
}

void rewind(FILE* f) {
  (void)fseek(f, 0L, SEEK_SET);
  LOCK();
  f->error_flag = 0;
  UNLOCK();
}

int fgetpos(FILE* f, fpos_t* pos) {
  long p = ftell(f);
  if (p < 0) return -1;
  *pos = p;
  return 0;
}

int fsetpos(FILE* f, const fpos_t* pos) {
  return fseek(f, (long)*pos, SEEK_SET);
}

/* ---- buffering control ----------------------------------------------------------- */

int setvbuf(FILE* f, char* buf, int mode, size_t size) {
  LOCK();
  if (f->fd < 0 || f->dir != 0 || f->buf_len != 0) {
    /* Too late or closed: the standard says the call is then
     * unspecified; refuse quietly with an error return. */
    UNLOCK();
    return -1;
  }
  if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
    errno = EINVAL;
    UNLOCK();
    return -1;
  }
  if (f->buf_owned) {
    free(f->buf);
    f->buf = NULL;
    f->buf_owned = 0;
  }
  f->bufmode = mode;
  if (mode == _IONBF) {
    f->buf = NULL;
    f->buf_size = 0;
  } else if (buf != NULL) {
    f->buf = (unsigned char*)buf;
    f->buf_size = size;
    f->buf_owned = 0;
  } else {
    size_t want = size > 0 ? size : BUFSIZ;
    f->buf = (unsigned char*)malloc(want);
    if (f->buf == NULL) {
      errno = ENOMEM;
      UNLOCK();
      return -1;
    }
    f->buf_size = want;
    f->buf_owned = 1;
  }
  UNLOCK();
  return 0;
}

void setbuf(FILE* f, char* buf) {
  (void)setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/* ---- status ------------------------------------------------------------------------- */

int feof(FILE* f) { return f->eof_flag; }
int ferror(FILE* f) { return f->error_flag; }
void clearerr(FILE* f) {
  LOCK();
  f->eof_flag = 0;
  f->error_flag = 0;
  UNLOCK();
}

void perror(const char* s) {
  int e = errno;
  const char* msg = strerror(e);
  if (s != NULL && *s != '\0') {
    (void)__litholibc_file_write(stderr, s, strlen(s));
    (void)__litholibc_file_write(stderr, ": ", 2);
  }
  (void)__litholibc_file_write(stderr, msg, strlen(msg));
  (void)__litholibc_file_write(stderr, "\n", 1);
}

/* ---- file system operations ---------------------------------------------------------- */

int remove(const char* path) {
  /* No unlink exists in the frozen surface (and the volume is
   * read-only by definition). Fail soft, documented. */
  (void)path;
  errno = ENOSYS;
  return -1;
}

int rename(const char* oldpath, const char* newpath) {
  (void)oldpath;
  (void)newpath;
  errno = ENOSYS;
  return -1;
}
