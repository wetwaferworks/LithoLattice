/* LithoLibC setenv/unsetenv/getenv over the lock-free overlay. Returns the
 * number of failed checks (0 = pass). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <waferheap.h>
static unsigned char g_arena[64 * 1024];
static void* grow_refused(wh_heap_t* heap, size_t min_extra, size_t* out, void* user) {
  (void)heap; (void)min_extra; (void)out; (void)user;
  return NULL;
}
int lk_env_check(void) {
  int bad = 0, i, r = 0;
  const char* v;
  wh_options_t opts;
  memset(&opts, 0, sizeof opts);
  opts.grow = grow_refused;
  wh_set_default_heap(wh_heap_create(g_arena, sizeof g_arena, &opts));
#define CK(c) do { if (!(c)) ++bad; } while (0)
  CK(getenv("LK_A") == NULL);
  CK(setenv("LK_A", "1", 0) == 0);
  v = getenv("LK_A"); CK(v && strcmp(v, "1") == 0);
  CK(setenv("LK_A", "2", 0) == 0);           /* no overwrite: keeps 1 */
  v = getenv("LK_A"); CK(v && strcmp(v, "1") == 0);
  CK(setenv("LK_A", "3", 1) == 0);           /* overwrite */
  CK(strcmp(getenv("LK_A"), "3") == 0);
  CK(strcmp(v, "1") == 0);                   /* the old pointer stays valid */
  CK(setenv("LK_AB", "x", 1) == 0);          /* prefix names stay apart */
  CK(strcmp(getenv("LK_A"), "3") == 0 && strcmp(getenv("LK_AB"), "x") == 0);
  CK(unsetenv("LK_A") == 0 && getenv("LK_A") == NULL && getenv("LK_AB") != NULL);
  CK(unsetenv("LK_NEVER") == 0);
  CK(setenv("LK_A", "4", 0) == 0 && strcmp(getenv("LK_A"), "4") == 0);
  CK(setenv("LK_E", "", 1) == 0 && getenv("LK_E") && getenv("LK_E")[0] == '\0');
  errno = 0; CK(setenv("", "1", 1) == -1 && errno == EINVAL);
  errno = 0; CK(setenv("A=B", "1", 1) == -1 && errno == EINVAL);
  errno = 0; CK(setenv(NULL, "1", 1) == -1 && errno == EINVAL);
  errno = 0; CK(unsetenv("A=B") == -1 && errno == EINVAL);
  for (i = 0; i < 80 && r == 0; i++) r = setenv("LK_FILL", "f", 1);  /* bounded overlay */
  CK(r == -1 && errno == ENOMEM);
  CK(strcmp(getenv("LK_A"), "4") == 0);      /* still readable when full */
  return bad;
}
