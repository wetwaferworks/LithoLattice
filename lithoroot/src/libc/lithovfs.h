/* lithovfs.h ... S7 LithoVFS, private to LithoLibC / LithoRoot C++. */
#ifndef LITHOVFS_H
#define LITHOVFS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LV_PATH_MAX 512
#define LV_FD_BASE 64   /* fds below are lithosys (0,1,2 stdio, volume fds) */
#define LV_FD_SLOTS 128
enum { LV_NONE = 0, LV_FILE = 1, LV_DIR = 2 };
typedef struct lv_stat { int type; int in_pack; uint64_t size; int64_t mtime_ns; } lv_stat_t;
/* All return 0 / a value >= 0, or -errno. Paths: any form; normalized. */
int lv_normalize(const char* in, char* out, size_t cap);
int lv_stat(const char* path, lv_stat_t* st);
int lv_open(const char* path, int oflags);
long long lv_read(int fd, void* buf, size_t n);
long long lv_write(int fd, const void* buf, size_t n);
long long lv_seek(int fd, long long off, int whence);
int lv_close(int fd);
int lv_truncate_fd(int fd, uint64_t size);
int lv_sync(int fd);
int lv_is_fd(int fd);
int lv_mkdir(const char* path);
int lv_remove(const char* path);
int lv_rename(const char* from, const char* to);
int lv_truncate_path(const char* path, uint64_t size);
/* Children of a directory, NUL-separated names into a malloc'd buffer
 * (*out, free() it); returns the count or -errno. */
int lv_list(const char* path, char** out);
#ifdef __cplusplus
}
#endif
#endif
