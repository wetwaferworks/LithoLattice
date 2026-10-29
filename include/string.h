/* string.h ... the LithoLibC string and memory surface.
 *
 * Standard-shaped declarations for exactly the functions the
 * LithoLibC core piece implements (src/libc). Anything not declared
 * here is not yet provided; the stdio / stdlib / math piece widens
 * other headers, not this one. Strict C99: no vendor extensions, no
 * platform headers.
 *
 * Notes on two residents:
 *  - strnlen is declared here although C99 did not standardize it
 *    (POSIX did, and C23 followed); the engine uses it and a bounded
 *    length is too useful to leave out.
 *  - strcoll and strxfrm are real implementations of the C locale
 *    semantics, not failure stubs: in the C locale collation order
 *    is codepoint order, so strcoll compares as strcmp does, and
 *    strxfrm copies as strncpy does and returns the full length.
 *    See docs/LITHOLIBC-CORE.md.
 */
#ifndef LITHOLIBC_STRING_H
#define LITHOLIBC_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- memory ---- */
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* dest, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);

/* ---- string ---- */
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strpbrk(const char* s, const char* accept);
char* strtok(char* s, const char* delim);
char* strtok_r(char* s, const char* delim,
               char** saveptr);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);
char* strerror(int errnum);
int strcoll(const char* a, const char* b);
size_t strxfrm(char* dest, const char* src, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_STRING_H */
