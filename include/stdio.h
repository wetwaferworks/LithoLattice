/* stdio.h ... the LithoLibC standard I/O header.
 *
 * Declares exactly what src/libc/stdio.c, printf.c and scanf.c
 * implement: the FILE layer over the frozen LithoSys fd surface
 * plus the read-only volume (docs/LITHOSYS-FREEZE.md, ruling
 * addendum), the printf family with exact float conversions, and
 * the scanf family the engine actually uses.
 *
 * Deliberately absent (no writable filesystem surface exists in
 * the freeze, and nothing calls them): tmpfile, tmpnam, and the
 * wide-character I/O family. remove and rename are declared and
 * fail soft with ENOSYS; see stdio.c.
 */
#ifndef LITHOLIBC_STDIO_H
#define LITHOLIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct litholibc_file FILE;
typedef long long fpos_t;

#ifndef EOF
#define EOF (-1)
#endif

#define BUFSIZ 4096
#define FOPEN_MAX 64
#define FILENAME_MAX 4096

#define _IONBF 0
#define _IOLBF 1
#define _IOFBF 2

/* Same values as the LithoSys seek constants, by design. */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

/* ---- file operations --------------------------------------------- */
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
int fflush(FILE* f);
void setbuf(FILE* f, char* buf);
int setvbuf(FILE* f, char* buf, int mode, size_t size);
int remove(const char* path);
int rename(const char* oldpath, const char* newpath);

/* ---- input / output ---------------------------------------------- */
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);
int fgetc(FILE* f);
int getc(FILE* f);
int getchar(void);
char* fgets(char* s, int n, FILE* f);
int ungetc(int c, FILE* f);
int fputc(int c, FILE* f);
int putc(int c, FILE* f);
int putchar(int c);
int fputs(const char* s, FILE* f);
int puts(const char* s);

/* ---- positioning --------------------------------------------------- */
int fseek(FILE* f, long offset, int whence);
long ftell(FILE* f);
void rewind(FILE* f);
int fgetpos(FILE* f, fpos_t* pos);
int fsetpos(FILE* f, const fpos_t* pos);

/* ---- status -------------------------------------------------------- */
int feof(FILE* f);
int ferror(FILE* f);
void clearerr(FILE* f);
void perror(const char* s);

/* ---- formatted output ---------------------------------------------- */
int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int sprintf(char* s, const char* fmt, ...);
int snprintf(char* s, size_t n, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vsprintf(char* s, const char* fmt, va_list ap);
int vsnprintf(char* s, size_t n, const char* fmt, va_list ap);

/* ---- formatted input ----------------------------------------------- */
int scanf(const char* fmt, ...);
int fscanf(FILE* f, const char* fmt, ...);
int sscanf(const char* s, const char* fmt, ...);
int vscanf(const char* fmt, va_list ap);
int vfscanf(FILE* f, const char* fmt, va_list ap);
int vsscanf(const char* s, const char* fmt, va_list ap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_STDIO_H */
