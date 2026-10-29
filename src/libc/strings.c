/* strings.c ... LithoLibC case-insensitive comparison.
 *
 * Case folding goes through our own tolower, so the comparison is
 * exactly the C locale's: ASCII letters fold, every other byte
 * compares as itself, as unsigned char.
 */

#include <strings.h>

#include <ctype.h>
#include <stddef.h>

int strcasecmp(const char* a, const char* b) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  for (;;) {
    const int cx = tolower(*x);
    const int cy = tolower(*y);
    if (cx != cy || *x == '\0') return cx - cy;
    ++x;
    ++y;
  }
}

int strncasecmp(const char* a, const char* b, size_t n) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  size_t i;
  for (i = 0; i < n; ++i) {
    const int cx = tolower(x[i]);
    const int cy = tolower(y[i]);
    if (cx != cy || x[i] == '\0') return cx - cy;
  }
  return 0;
}
