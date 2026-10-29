/* errno.h ... the LithoLibC error number.
 *
 * errno is a macro over a per-thread slot accessor, in the shape
 * the C standard has allowed since C11 made errno thread-local in
 * intent. The accessor is the whole design:
 *
 *   int* __litholibc_errno_location(void);
 *
 * Today, with no thread library wired, the implementation returns
 * the address of one static slot, so errno behaves as the classic
 * process-wide global. When LithoThread lands it swaps the
 * accessor's body for a true TLS lookup; no caller recompiles and
 * the ABI does not move, because no caller ever took the address
 * of a global named errno... they only ever called the accessor
 * through this macro. The indirection costs one call on each
 * access, paid only by code that touches errno at all.
 */
#ifndef LITHOLIBC_ERRNO_H
#define LITHOLIBC_ERRNO_H

#include <litholibc/errno_values.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Address of the calling thread's errno slot. Never returns NULL. */
int* __litholibc_errno_location(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#define errno (*__litholibc_errno_location())

#endif /* LITHOLIBC_ERRNO_H */
