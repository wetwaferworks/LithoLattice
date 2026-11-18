/* LithoRoot <fcntl.h>: open flags and open/fcntl, declarations only
 * (definitions: LithoVFS fd layer, S7). Flag values follow Linux/musl. */
#ifndef LITHOROOT_FCNTL_H
#define LITHOROOT_FCNTL_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define O_RDONLY    00
#define O_WRONLY    01
#define O_RDWR      02
#define O_ACCMODE   03
#define O_CREAT     0100
#define O_EXCL      0200
#define O_NOCTTY    0400
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DSYNC     010000
#define O_SYNC      04010000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_CLOEXEC   02000000
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int open(const char* path, int flags, ...);
int fcntl(int fd, int cmd, ...);
#ifdef __cplusplus
}
#endif
#endif
