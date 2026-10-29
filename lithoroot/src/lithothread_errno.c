/* lithothread_errno.c ... the real body of Piece 3's errno accessor.
 *
 * Piece 3 left __litholibc_errno_location returning one static
 * slot (src/libc/errno.c) behind an indirection built for this
 * day: "when LithoThread lands, the accessor's body becomes a
 * true TLS lookup and nothing else moves." This file is that
 * body. It provides the per-thread slot: each record carries its
 * own errno value, and the accessor resolves it through the
 * calling thread's record pointer.
 *
 * LINK RULE (the coordination with Piece 3, chosen deliberately
 * over weak symbols: MSVC has none, and wasm link behavior for
 * weak definitions in archives is the murky kind of clever that
 * fails at 2 a.m.): a link that includes LithoThread EXCLUDES
 * src/libc/errno.c. A link without LithoThread keeps errno.c and
 * gets the classic process-wide errno. Exactly one definition of
 * __litholibc_errno_location exists in any link, strongly, by
 * construction. The test builds in docs/LITHOTHREAD.md follow
 * the rule; the errno test case there proves the per-thread
 * behavior of this body while linking errno.c's absence.
 *
 * The arming flag: until startup calls
 * __litholibc_errno_arm_tls (after the default heap exists and
 * the main thread is attached), the accessor returns the plain
 * fallback slot and never calls into LithoThread. That keeps
 * early-boot and loader-time errno writes (the sanitizer-init
 * dlsym path Pieces 3 and 4 met) on exactly the code path
 * errno.c provides today. After arming, a thread with no record
 * attaches lazily through lt__attach_rec, whose own recursion
 * guard answers allocation failure with this same fallback.
 *
 * The thread-local record pointer lives here, in a deliberately
 * boring translation unit (no sanitizer instrumentation in the
 * documented sanitizer builds, like the other interposition
 * shims): lithothread.c's trampoline and attach path write it,
 * the accessor's fast path reads it, and a thread's pointer dies
 * with the thread.
 */

#include <errno.h>

#include "lithothread_internal.h"

LT__THREAD_LOCAL lt_rec_t* lt__current_rec;

static int lt__errno_fallback;
static volatile uint32_t lt__errno_armed;

void __litholibc_errno_arm_tls(void) {
  lt_at_store32(&lt__errno_armed, 1u);
}

int* __litholibc_errno_location(void) {
  lt_rec_t* rec = lt__current_rec;
  if (rec != NULL) return &rec->errno_value;
  if (lt_at_load32(&lt__errno_armed) == 0u) return &lt__errno_fallback;
  rec = lt__attach_rec();
  if (rec != NULL) return &rec->errno_value;
  return &lt__errno_fallback;
}
