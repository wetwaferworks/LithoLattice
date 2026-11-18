/* LithoRoot <sys/random.h>: getrandom over lithosys_random_fill (defined here;
 * the frozen LithoSys surface already has the host randomness call). */
#ifndef LITHOROOT_SYS_RANDOM_H
#define LITHOROOT_SYS_RANDOM_H
#include <sys/types.h>
#include <errno.h>
#include <lithosys.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
static inline ssize_t getrandom(void* buf, size_t len, unsigned flags) {
  (void)flags;
  if (len > 0x7fffffffu) len = 0x7fffffffu;
  if (lithosys_random_fill(buf, (uint32_t)len) != LITHOSYS_OK) { errno = ENOSYS; return -1; }
  return (ssize_t)len;
}
#endif
