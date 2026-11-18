/* lithovfs.c ... S7: LithoVFS, the writable overlay over the read-only
 * pack, persisted through lk_store (include/lk_store.h). Web only.
 *
 * No locks (JD's doctrine):
 *  - fd table: LV_FD_SLOTS slots claimed by CAS on a state word; fd =
 *    LV_FD_BASE + slot. fds 0, 1, 2 stay lithosys stdio and volume fds
 *    stay lithosys fds, both below LV_FD_BASE.
 *  - the tree: an immutable snapshot (sorted array of {path, type}
 *    for every file and directory in the store) behind one atomic
 *    pointer. A mutation calls lk_store first, then builds a new
 *    snapshot from the current one and publishes it by CAS (retry
 *    on conflict). Readers announce themselves on an atomic counter
 *    before loading the pointer; replaced snapshots go on a retired
 *    list, freed only after the list is taken AND the reader count is
 *    seen at zero (seq-cst), which proves no reader still holds one.
 *    Path strings are interned for the process life (snapshots share
 *    them), so only arrays are ever freed.
 *  - lookup order: snapshot (store) first, then the pack. The store
 *    shadows the pack; the pack is never written. Opening a pack-only
 *    file to modify it (r+, a, a+) is EROFS; a truncating open (w,
 *    w+) creates a store file that shadows it.
 * File data lives in lk_store only; sizes come from lk_store_stat. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "lithosys.h"
#include "lk_store.h"
#include "lithovfs.h"

#define ACQ __ATOMIC_ACQUIRE
#define REL __ATOMIC_RELEASE
#define SC __ATOMIC_SEQ_CST

/* ---- paths ---------------------------------------------------------- */
int lv_normalize(const char* in, char* out, size_t cap) {
  size_t o = 0;
  const char* p = in;
  if (in == NULL || cap < 2) return -EINVAL;
  if (*in == '\0') return -ENOENT;
  out[o++] = '/'; /* relative paths resolve against cwd "/" */
  while (*p) {
    const char* s;
    size_t n;
    while (*p == '/') ++p;
    s = p;
    while (*p && *p != '/') ++p;
    n = (size_t)(p - s);
    if (n == 0 || (n == 1 && s[0] == '.')) continue;
    if (n == 2 && s[0] == '.' && s[1] == '.') {
      if (o > 1) {
        --o;
        while (o > 1 && out[o - 1] != '/') --o;
        if (o > 1) --o;
      }
      continue;
    }
    if (o > 1) out[o++] = '/';
    if (o + n + 1 > cap) return -ENAMETOOLONG;
    memcpy(out + o, s, n);
    o += n;
  }
  out[o] = '\0';
  return (int)o;
}

static const char* lv__parent_end(const char* p) { /* last '/' */
  const char* s = strrchr(p, '/');
  return s;
}

/* ---- snapshots -------------------------------------------------------- */
typedef struct lv_ent { const char* path; int type; } lv_ent_t;
typedef struct lv_snap {
  struct lv_snap* retired_next;
  size_t n;
  lv_ent_t e[];
} lv_snap_t;

static lv_snap_t* lv__root;     /* NULL until hydrated */
static unsigned lv__readers;
static lv_snap_t* lv__retired;

static lv_snap_t* lv__snap_new(size_t n) {
  lv_snap_t* s = (lv_snap_t*)malloc(sizeof(lv_snap_t) + n * sizeof(lv_ent_t));
  if (s) { s->n = n; s->retired_next = NULL; }
  return s;
}

static int lv__cmp(const char* a, const char* b) { return strcmp(a, b); }

static long lv__find(const lv_snap_t* s, const char* path) {
  size_t lo = 0, hi = s->n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = lv__cmp(s->e[mid].path, path);
    if (c == 0) return (long)mid;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return -(long)lo - 1;
}

static void lv__reclaim(void) {
  lv_snap_t* list = __atomic_exchange_n(&lv__retired, NULL, SC);
  if (list == NULL) return;
  if (__atomic_load_n(&lv__readers, SC) == 0u) {
    while (list) { lv_snap_t* n = list->retired_next; free(list); list = n; }
    return;
  }
  { /* readers active: hand the list back */
    lv_snap_t* tail = list;
    lv_snap_t* head;
    while (tail->retired_next) tail = tail->retired_next;
    head = __atomic_load_n(&lv__retired, SC);
    do { tail->retired_next = head; }
    while (!__atomic_compare_exchange_n(&lv__retired, &head, list, 1, SC, SC));
  }
}

static void lv__retire(lv_snap_t* s) {
  lv_snap_t* head;
  if (s == NULL) return;
  head = __atomic_load_n(&lv__retired, SC);
  do { s->retired_next = head; }
  while (!__atomic_compare_exchange_n(&lv__retired, &head, s, 1, SC, SC));
  lv__reclaim();
}

static const char* lv__intern(const char* p) {
  size_t n = strlen(p) + 1;
  char* c = (char*)malloc(n);
  if (c) memcpy(c, p, n);
  return c;
}

/* Hydrate: walk the store once. A thread that loses the publish race
 * frees its own copy; nobody waits for anybody. */
static int lv__walk(const char* dir, lv_ent_t** v, size_t* n, size_t* cap) {
  char small[1024];
  char* buf = small;
  int32_t need = lk_store_list(dir, (uint32_t)strlen(dir), small, sizeof small);
  char* q;
  if (need < 0) return need == -ENOENT ? 0 : need;
  if ((size_t)need > sizeof small) {
    buf = (char*)malloc((size_t)need);
    if (!buf) return -ENOMEM;
    need = lk_store_list(dir, (uint32_t)strlen(dir), buf, (uint32_t)need);
    if (need < 0) { free(buf); return need; }
  }
  for (q = buf; q < buf + need; q += strlen(q) + 1) {
    char full[LV_PATH_MAX];
    size_t ln = strlen(q);
    int is_dir = ln > 0 && q[ln - 1] == '/';
    if (ln == 0) continue;
    if (strlen(dir) + ln + 2 > sizeof full) continue;
    strcpy(full, dir);
    if (strcmp(dir, "/") != 0) strcat(full, "/");
    strncat(full, q, is_dir ? ln - 1 : ln);
    if (*n == *cap) {
      size_t nc = *cap ? *cap * 2 : 64;
      lv_ent_t* nv = (lv_ent_t*)realloc(*v, nc * sizeof(lv_ent_t));
      if (!nv) break;
      *v = nv; *cap = nc;
    }
    (*v)[*n].path = lv__intern(full);
    (*v)[*n].type = is_dir ? LV_DIR : LV_FILE;
    ++*n;
    if (is_dir) lv__walk(full, v, n, cap);
  }
  if (buf != small) free(buf);
  return 0;
}

static int lv__ent_cmp(const void* a, const void* b) {
  return strcmp(((const lv_ent_t*)a)->path, ((const lv_ent_t*)b)->path);
}

static lv_snap_t* lv__acquire(void) { /* caller must lv__release */
  lv_snap_t* s;
  __atomic_add_fetch(&lv__readers, 1u, SC);
  s = __atomic_load_n(&lv__root, SC);
  if (s == NULL) {
    lv_ent_t* v = NULL;
    size_t n = 0, cap = 0;
    lv_snap_t* fresh;
    lv_snap_t* expect = NULL;
    (void)lv__walk("/", &v, &n, &cap);
    qsort(v, n, sizeof(lv_ent_t), lv__ent_cmp);
    fresh = lv__snap_new(n);
    if (fresh == NULL) { free(v); return NULL; }
    if (n) memcpy(fresh->e, v, n * sizeof(lv_ent_t));
    free(v);
    if (__atomic_compare_exchange_n(&lv__root, &expect, fresh, 0, SC, SC)) s = fresh;
    else { free(fresh); s = expect; }
  }
  return s;
}

static void lv__release(void) { __atomic_sub_fetch(&lv__readers, 1u, SC); }

/* Publish root' = root with path set to type (LV_NONE removes; a
 * rename passes the prefix map). Retries on CAS conflict. */
static int lv__publish_edit(const char* path, int type, const char* rename_to) {
  for (;;) {
    lv_snap_t* cur = lv__acquire();
    lv_snap_t* nx;
    size_t i, k = 0, plen = strlen(path);
    size_t moved = 0, keep;
    lv_ent_t* tail = NULL;
    if (cur == NULL) { lv__release(); return -ENOMEM; }
    nx = lv__snap_new(cur->n + 1);
    if (nx == NULL) { lv__release(); return -ENOMEM; }
    /* Entries that stay keep their sorted order. New / renamed entries go to
     * the tail; one tail entry (create, file rename: the common case, e.g.
     * every writeFileAtomic backup rotation) is placed by binary search, more
     * (a directory rename) fall back to qsort. Same snapshot as before, but no
     * O(n log n) strcmp sort per edit on a ~6k-entry tree (web save hitches). */
    for (i = 0; i < cur->n; ++i) {
      const char* p = cur->e[i].path;
      int hit = strcmp(p, path) == 0;
      int under = strncmp(p, path, plen) == 0 && p[plen] == '/';
      if (rename_to != NULL && (hit || under)) {
        char np[LV_PATH_MAX];
        if (strlen(rename_to) + strlen(p + plen) + 1 > sizeof np) continue;
        strcpy(np, rename_to);
        strcat(np, p + plen);
        if (moved == 0 || (moved & (moved - 1)) == 0) {
          lv_ent_t* nt = (lv_ent_t*)realloc(tail, (moved ? moved * 2 : 1) * sizeof(lv_ent_t));
          if (nt == NULL) continue;
          tail = nt;
        }
        tail[moved].path = lv__intern(np);
        tail[moved++].type = cur->e[i].type;
        continue;
      }
      if (hit || (type == LV_NONE && under && rename_to == NULL)) continue;
      nx->e[k++] = cur->e[i];
    }
    if (type != LV_NONE && rename_to == NULL) {
      lv_ent_t* nt = (lv_ent_t*)realloc(tail, sizeof(lv_ent_t));
      if (nt != NULL) { tail = nt; tail[0].path = lv__intern(path); tail[0].type = type; moved = 1; }
    }
    keep = k;
    if (moved > 0 && keep + moved > cur->n + 1) { /* grow: a rename can only re-key, never add */
      lv_snap_t* g = lv__snap_new(keep + moved);
      if (g == NULL) { free(nx); free(tail); lv__release(); return -ENOMEM; }
      if (keep) memcpy(g->e, nx->e, keep * sizeof(lv_ent_t));
      free(nx);
      nx = g;
    }
    if (moved == 1) {
      size_t lo = 0, hi = keep;
      while (lo < hi) { size_t mid = (lo + hi) / 2; if (strcmp(nx->e[mid].path, tail[0].path) < 0) lo = mid + 1; else hi = mid; }
      memmove(&nx->e[lo + 1], &nx->e[lo], (keep - lo) * sizeof(lv_ent_t));
      nx->e[lo] = tail[0];
      k = keep + 1;
    } else if (moved > 1) {
      memcpy(&nx->e[keep], tail, moved * sizeof(lv_ent_t));
      k = keep + moved;
      qsort(nx->e, k, sizeof(lv_ent_t), lv__ent_cmp);
    }
    free(tail);
    nx->n = k;
    {
      lv_snap_t* expect = cur;
      int ok = __atomic_compare_exchange_n(&lv__root, &expect, nx, 0, SC, SC);
      lv__release();
      if (ok) { lv__retire(cur); return 0; }
      free(nx);
    }
  }
}

static int lv__overlay_type(const char* norm) {
  lv_snap_t* s = lv__acquire();
  int t = LV_NONE;
  if (s) {
    long i = lv__find(s, norm);
    if (i >= 0) t = s->e[i].type;
    else if (strcmp(norm, "/") == 0) t = LV_DIR;
  }
  lv__release();
  return t;
}

/* ---- stat --------------------------------------------------------------- */
static int lv__pack_probe(const char* norm, uint64_t* size) {
  uint32_t fd = 0;
  uint64_t end = 0;
  if (lithosys_volume_open(norm + 1, &fd) != LITHOSYS_OK) return 0;
  if (lithosys_fd_seek(fd, 0, 2, &end) != LITHOSYS_OK) end = 0;
  (void)lithosys_fd_close(fd);
  *size = end;
  return 1;
}

int lv_stat(const char* path, lv_stat_t* st) {
  char n[LV_PATH_MAX];
  int r = lv_normalize(path, n, sizeof n), t;
  memset(st, 0, sizeof *st);
  if (r < 0) return r;
  t = lv__overlay_type(n);
  if (t == LV_DIR) { st->type = LV_DIR; return 0; }
  if (t == LV_FILE) {
    lk_store_stat_t ss;
    if (lk_store_stat(n, (uint32_t)strlen(n), &ss) == 0) {
      st->size = ss.size;
      st->mtime_ns = ss.mtime_ns;
    }
    st->type = LV_FILE;
    return 0;
  }
  if (lv__pack_probe(n, &st->size)) { st->type = LV_FILE; st->in_pack = 1; return 0; }
  return -ENOENT;
}

/* ---- fd table ------------------------------------------------------------ */
typedef struct lv_fd {
  uint32_t state; /* 0 free, 1 claimed; CAS */
  int32_t h;      /* lk_store handle */
  int pack_fd;    /* >= 0: a read-only pack fd (lithosys) */
  int append;
  char path[LV_PATH_MAX];
  long long pos;  /* only the fd's user moves it (POSIX leaves shared use undefined) */
} lv_fd_t;
static lv_fd_t lv__fds[LV_FD_SLOTS];

static lv_fd_t* lv__fd(int fd) {
  lv_fd_t* f;
  if (fd < LV_FD_BASE || fd >= LV_FD_BASE + LV_FD_SLOTS) return NULL;
  f = &lv__fds[fd - LV_FD_BASE];
  return __atomic_load_n(&f->state, ACQ) ? f : NULL;
}
int lv_is_fd(int fd) { return lv__fd(fd) != NULL; }

static int lv__claim(void) {
  int i;
  for (i = 0; i < LV_FD_SLOTS; ++i) {
    uint32_t e = 0;
    if (__atomic_compare_exchange_n(&lv__fds[i].state, &e, 1u, 0, ACQ, ACQ)) return i;
  }
  return -EMFILE;
}

static int lv__ensure_parent(const char* n) {
  char par[LV_PATH_MAX];
  const char* e = lv__parent_end(n);
  size_t len = (size_t)(e - n);
  if (len == 0) return 0; /* parent is "/" */
  memcpy(par, n, len); par[len] = '\0';
  return lv__overlay_type(par) == LV_DIR ? 0 : -ENOENT;
}

int lv_open(const char* path, int oflags) {
  char n[LV_PATH_MAX];
  int r = lv_normalize(path, n, sizeof n), slot, t;
  int acc = oflags & O_ACCMODE;
  uint32_t fl = 0;
  if (r < 0) return r;
  t = lv__overlay_type(n);
  if (t == LV_DIR) return -EISDIR;
  slot = lv__claim();
  if (slot < 0) return slot;
  lv__fds[slot].pos = 0;
  lv__fds[slot].append = (oflags & O_APPEND) != 0;
  lv__fds[slot].pack_fd = -1;
  lv__fds[slot].h = -1;
  if (t == LV_NONE && acc == O_RDONLY) {
    uint32_t vfd;
    if (lithosys_volume_open(n + 1, &vfd) == LITHOSYS_OK) {
      lv__fds[slot].pack_fd = (int)vfd;
      return LV_FD_BASE + slot;
    }
    __atomic_store_n(&lv__fds[slot].state, 0u, REL);
    return -ENOENT;
  }
  if (t == LV_NONE) {
    uint64_t psz;
    /* A pack-only file opened to modify its contents (r+, a, a+) is
     * read-only: EROFS, not ENOENT. Only a truncating open (w, w+)
     * may shadow it with a fresh store file. */
    if (!(oflags & O_TRUNC) && lv__pack_probe(n, &psz)) {
      __atomic_store_n(&lv__fds[slot].state, 0u, REL);
      return -EROFS;
    }
    if (!(oflags & O_CREAT)) { __atomic_store_n(&lv__fds[slot].state, 0u, REL); return -ENOENT; }
    if ((r = lv__ensure_parent(n)) < 0) { __atomic_store_n(&lv__fds[slot].state, 0u, REL); return r; }
  }
  fl = (acc == O_WRONLY ? LK_STORE_WRITE : acc == O_RDWR ? (LK_STORE_READ | LK_STORE_WRITE) : LK_STORE_READ);
  if (oflags & O_CREAT) fl |= LK_STORE_CREATE;
  if (oflags & O_TRUNC) fl |= LK_STORE_TRUNC;
  if (oflags & O_EXCL) fl |= LK_STORE_EXCL;
  r = lk_store_open(n, (uint32_t)strlen(n), fl);
  if (r < 0) { __atomic_store_n(&lv__fds[slot].state, 0u, REL); return r; }
  lv__fds[slot].h = r;
  strcpy(lv__fds[slot].path, n);
  if (t == LV_NONE) (void)lv__publish_edit(n, LV_FILE, NULL);
  return LV_FD_BASE + slot;
}

long long lv_read(int fd, void* buf, size_t len) {
  lv_fd_t* f = lv__fd(fd);
  int32_t r;
  if (!f) return -EBADF;
  if (f->pack_fd >= 0) {
    lithosys_iov_t iov;
    uint32_t got = 0;
    lithosys_err_t e;
    iov.buf = buf; iov.len = (uint32_t)len;
    e = lithosys_fd_read((uint32_t)f->pack_fd, &iov, 1, &got);
    if (e != LITHOSYS_OK) return -(long long)e;
    f->pos += got;
    return got;
  }
  r = lk_store_read(f->h, buf, (uint32_t)len, (uint64_t)f->pos);
  if (r > 0) f->pos += r;
  return r;
}

long long lv_write(int fd, const void* buf, size_t len) {
  lv_fd_t* f = lv__fd(fd);
  int32_t r;
  if (!f) return -EBADF;
  if (f->pack_fd >= 0) return -EBADF;
  if (f->append) {
    lk_store_stat_t ss;
    if (lk_store_stat(f->path, (uint32_t)strlen(f->path), &ss) == 0) f->pos = (long long)ss.size;
  }
  r = lk_store_write(f->h, buf, (uint32_t)len, (uint64_t)f->pos);
  if (r > 0) f->pos += r;
  return r;
}

long long lv_seek(int fd, long long off, int whence) {
  lv_fd_t* f = lv__fd(fd);
  long long base = 0;
  if (!f) return -EBADF;
  if (f->pack_fd >= 0) {
    uint64_t np = 0;
    lithosys_err_t e = lithosys_fd_seek((uint32_t)f->pack_fd, off, (uint32_t)whence, &np);
    if (e != LITHOSYS_OK) return -(long long)e;
    f->pos = (long long)np;
    return f->pos;
  }
  if (whence == 1) base = f->pos;
  else if (whence == 2) {
    lk_store_stat_t ss;
    int32_t r = lk_store_stat(f->path, (uint32_t)strlen(f->path), &ss);
    if (r < 0) return r;
    base = (long long)ss.size;
  } else if (whence != 0) return -EINVAL;
  if (base + off < 0) return -EINVAL;
  f->pos = base + off;
  return f->pos;
}

int lv_close(int fd) {
  lv_fd_t* f = lv__fd(fd);
  int r = 0;
  if (!f) return -EBADF;
  if (f->pack_fd >= 0) (void)lithosys_fd_close((uint32_t)f->pack_fd);
  else r = lk_store_close(f->h);
  __atomic_store_n(&f->state, 0u, REL);
  return r < 0 ? r : 0;
}

int lv_truncate_fd(int fd, uint64_t size) {
  lv_fd_t* f = lv__fd(fd);
  if (!f) return -EBADF;
  if (f->pack_fd >= 0) return -EINVAL; /* read-only pack file */
  return lk_store_truncate(f->h, size);
}

int lv_sync(int fd) {
  lv_fd_t* f = lv__fd(fd);
  if (!f) return -EBADF;
  if (f->pack_fd >= 0) return 0;
  return lk_store_flush(f->h);
}

int lv_mkdir(const char* path) {
  char n[LV_PATH_MAX];
  int r = lv_normalize(path, n, sizeof n);
  if (r < 0) return r;
  if (strcmp(n, "/") == 0 || lv__overlay_type(n) != LV_NONE) return -EEXIST;
  if ((r = lv__ensure_parent(n)) < 0) return r;
  r = lk_store_mkdir(n, (uint32_t)strlen(n));
  if (r < 0 && r != -EEXIST) return r;
  (void)lv__publish_edit(n, LV_DIR, NULL);
  return r;
}

int lv_remove(const char* path) {
  char n[LV_PATH_MAX];
  int r = lv_normalize(path, n, sizeof n), t;
  if (r < 0) return r;
  t = lv__overlay_type(n);
  if (t == LV_NONE) {
    uint64_t sz;
    return lv__pack_probe(n, &sz) ? -EROFS : -ENOENT;
  }
  r = lk_store_remove(n, (uint32_t)strlen(n));
  if (r < 0) return r;
  return lv__publish_edit(n, LV_NONE, NULL);
}

int lv_rename(const char* from, const char* to) {
  char a[LV_PATH_MAX], b[LV_PATH_MAX];
  int r;
  if ((r = lv_normalize(from, a, sizeof a)) < 0) return r;
  if ((r = lv_normalize(to, b, sizeof b)) < 0) return r;
  if (lv__overlay_type(a) == LV_NONE) {
    uint64_t sz;
    return lv__pack_probe(a, &sz) ? -EROFS : -ENOENT;
  }
  if ((r = lv__ensure_parent(b)) < 0) return r;
  r = lk_store_rename(a, (uint32_t)strlen(a), b, (uint32_t)strlen(b));
  if (r < 0) return r;
  (void)lv__publish_edit(b, LV_NONE, NULL); /* replaced target */
  return lv__publish_edit(a, LV_FILE, b);
}

int lv_truncate_path(const char* path, uint64_t size) {
  int fd = lv_open(path, O_WRONLY), r;
  if (fd < 0) return fd;
  r = lv_truncate_fd(fd, size);
  (void)lv_close(fd);
  return r;
}

int lv_list(const char* path, char** out) {
  char n[LV_PATH_MAX];
  int r = lv_normalize(path, n, sizeof n), count = 0;
  size_t plen, total = 0, i;
  lv_snap_t* s;
  char* buf;
  *out = NULL;
  if (r < 0) return r;
  if (lv__overlay_type(n) != LV_DIR) return lv__overlay_type(n) == LV_FILE ? -ENOTDIR : -ENOENT;
  plen = strcmp(n, "/") == 0 ? 0 : strlen(n);
  s = lv__acquire();
  if (!s) { lv__release(); return -ENOMEM; }
  for (i = 0; i < s->n; ++i) {
    const char* p = s->e[i].path;
    if (strncmp(p, n, plen) == 0 && p[plen] == '/' && strchr(p + plen + 1, '/') == NULL)
      total += strlen(p + plen + 1) + 1;
  }
  buf = (char*)malloc(total + 1);
  if (buf) {
    char* w = buf;
    for (i = 0; i < s->n; ++i) {
      const char* p = s->e[i].path;
      if (strncmp(p, n, plen) == 0 && p[plen] == '/' && strchr(p + plen + 1, '/') == NULL) {
        size_t l = strlen(p + plen + 1) + 1;
        memcpy(w, p + plen + 1, l); w += l; ++count;
      }
    }
  }
  lv__release();
  if (!buf) return -ENOMEM;
  *out = buf;
  return count;
}

/* ---- POSIX over the fd table ---------------------------------------- */
#include <unistd.h>
int isatty(int fd) {
  if (fd >= 0 && fd <= 2) return 1;
  errno = (lv_is_fd(fd) || fd >= 0) ? ENOTSUP : EBADF;
  return 0;
}
int fsync(int fd) {
  int r;
  if (fd >= 0 && fd <= 2) return lithosys_fd_sync((uint32_t)fd) == LITHOSYS_OK ? 0 : (errno = EIO, -1);
  r = lv_sync(fd);
  if (r < 0) { errno = -r; return -1; }
  return 0;
}
int ftruncate(int fd, off_t len) {
  int r;
  if (len < 0) { errno = EINVAL; return -1; }
  r = lv_truncate_fd(fd, (uint64_t)len);
  if (r < 0) { errno = -r; return -1; }
  return 0;
}
