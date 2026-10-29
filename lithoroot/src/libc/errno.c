/* errno.c ... the LithoLibC errno slot and its accessor.
 *
 * One static slot behind __litholibc_errno_location, exactly the
 * indirection errno.h documents: while no thread library is wired
 * this is the classic process-wide errno, and when LithoThread
 * lands the body of the accessor changes to a TLS lookup and
 * nothing else in the library, or in any caller, moves.
 */

#include <errno.h>

/* Review L9: a link with LithoThread takes the accessor from
 * src/lithothread_errno.c. Builds that link LithoThread define
 * LITHOROOT_WITH_THREAD, so this definition cannot collide with it
 * even if both TUs are compiled. */
#ifndef LITHOROOT_WITH_THREAD
static int lk_errno_slot;

int* __litholibc_errno_location(void) { return &lk_errno_slot; }
#else
typedef int lk_errno_c_empty_tu; /* ISO C: no empty TU */
#endif
