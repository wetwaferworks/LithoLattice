/* LithoRoot <unistd.h>: POSIX fd calls, declarations only.
 * The definitions belong to the LithoVFS fd layer (S7, plan 3.8); until it lands
 * a web link that reaches them reports them as undefined. getpid() is defined:
 * the web build is one process, pid 1 (LithoSys has no process ids). */
#ifndef LITHOROOT_UNISTD_H
#define LITHOROOT_UNISTD_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int fsync(int fd);
int fdatasync(int fd);
int ftruncate(int fd, off_t len);
int unlink(const char* path);
int rmdir(const char* path);
int isatty(int fd);
int usleep(unsigned usec);
static inline pid_t getpid(void) { return 1; }
#ifdef __cplusplus
}
#endif
#endif
