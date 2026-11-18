/* lk_store.h ... S7: the host storage calls behind LithoVFS (web only).
 *
 * JD, 2026-11-16 12:41 PM: "Saves: yes, add in-house lk_store calls
 * backed by OPFS, keep the 14-call surface frozen." These are lkboot
 * lk_* imports (module "env"), NOT lithosys calls; the lithosys
 * surface stays at 14. lkboot backs them with the Origin Private
 * File System; the Node tests back them with an in-memory map.
 *
 * Conventions, every call:
 *  - paths are UTF-8, absolute, normalized by LithoVFS ("/saves/a"),
 *    passed as pointer + byte length (no NUL needed);
 *  - the return is >= 0 on success, or a negated LITHOSYS_E* code
 *    (-LITHOSYS_ENOENT, -LITHOSYS_EIO, -LITHOSYS_ENOSPC, ...);
 *  - offsets and sizes are 64-bit (BigInt on the JS side);
 *  - calls may block (OPFS sync access handles live in workers);
 *    LithoVFS never calls them from the page thread's frame loop.
 *
 * The set is minimal on purpose: handles do positioned I/O only (the
 * file position lives in LithoVFS's fd table), and the namespace is
 * flat paths plus explicit directories.
 */
#ifndef LK_STORE_H
#define LK_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__)
#define LK_STORE_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#else
#define LK_STORE_IMPORT(name)
#endif

/* open flags */
#define LK_STORE_READ 0x1u
#define LK_STORE_WRITE 0x2u
#define LK_STORE_CREATE 0x4u  /* create when absent (parent must exist) */
#define LK_STORE_TRUNC 0x8u   /* size 0 after open */
#define LK_STORE_EXCL 0x10u   /* with CREATE: fail with EEXIST if present */

/* stat types */
#define LK_STORE_T_FILE 1u
#define LK_STORE_T_DIR 2u

typedef struct lk_store_stat {
  uint32_t type;      /* LK_STORE_T_* */
  uint32_t reserved;
  uint64_t size;      /* bytes, files only */
  int64_t mtime_ns;   /* last write, realtime ns */
} lk_store_stat_t;    /* 24 bytes, little endian, as written by the host */

/* Open a file. Returns a handle (>= 0) or -err. */
LK_STORE_IMPORT(lk_store_open)
int32_t lk_store_open(const char* path, uint32_t path_len, uint32_t flags);
/* Positioned read / write. Return bytes moved or -err. */
LK_STORE_IMPORT(lk_store_read)
int32_t lk_store_read(int32_t h, void* buf, uint32_t len, uint64_t off);
LK_STORE_IMPORT(lk_store_write)
int32_t lk_store_write(int32_t h, const void* buf, uint32_t len, uint64_t off);
/* Set the size (grow zero-fills). 0 or -err. */
LK_STORE_IMPORT(lk_store_truncate)
int32_t lk_store_truncate(int32_t h, uint64_t size);
/* Make everything written durable (OPFS flush). 0 or -err. */
LK_STORE_IMPORT(lk_store_flush)
int32_t lk_store_flush(int32_t h);
LK_STORE_IMPORT(lk_store_close)
int32_t lk_store_close(int32_t h);
/* Metadata by path. 0 or -err (ENOENT when absent). */
LK_STORE_IMPORT(lk_store_stat)
int32_t lk_store_stat(const char* path, uint32_t path_len, lk_store_stat_t* out);
/* Create one directory (parent must exist). 0, or -EEXIST, or -err. */
LK_STORE_IMPORT(lk_store_mkdir)
int32_t lk_store_mkdir(const char* path, uint32_t path_len);
/* List a directory's direct children into buf as NUL-terminated
 * names, a directory's name ending in '/'. Returns the bytes the full
 * list needs (call again with a bigger buf when it exceeds cap). */
LK_STORE_IMPORT(lk_store_list)
int32_t lk_store_list(const char* path, uint32_t path_len, char* buf, uint32_t cap);
/* Remove a file or an EMPTY directory. 0 or -err. */
LK_STORE_IMPORT(lk_store_remove)
int32_t lk_store_remove(const char* path, uint32_t path_len);
/* Rename, replacing a file at the target. 0 or -err. */
LK_STORE_IMPORT(lk_store_rename)
int32_t lk_store_rename(const char* from, uint32_t from_len, const char* to, uint32_t to_len);

#ifdef __cplusplus
}
#endif
#endif
