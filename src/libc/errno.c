/* errno.c ... the LithoLibC errno slot and its accessor.
 *
 * One static slot behind __litholibc_errno_location, exactly the
 * indirection errno.h documents: while no thread library is wired
 * this is the classic process-wide errno, and when LithoThread
 * lands the body of the accessor changes to a TLS lookup and
 * nothing else in the library, or in any caller, moves.
 */

#include <errno.h>

static int lk_errno_slot;

int* __litholibc_errno_location(void) { return &lk_errno_slot; }
