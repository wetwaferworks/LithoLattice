/* strings.h ... the POSIX strings surface LithoLibC provides.
 *
 * Only the two case-insensitive comparisons live here in this
 * piece; the historical bit-string functions (ffs and family) are
 * deliberately not declared until something calls them.
 */
#ifndef LITHOLIBC_STRINGS_H
#define LITHOLIBC_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_STRINGS_H */
