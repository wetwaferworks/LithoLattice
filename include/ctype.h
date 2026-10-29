/* ctype.h ... the LithoLibC character classification surface.
 *
 * C locale only. The functions are real functions (no macro forms),
 * so their addresses can be taken and no argument is evaluated
 * twice. The domain promise is wider than the standard's minimum:
 * the classification is defined for every int input. EOF (-1)
 * classifies as nothing and converts to itself; other negative
 * values classify as the byte with the same low eight bits, which
 * is what passing a plain signed char yields on every platform
 * LithoRoot targets; values above 255 classify as nothing and
 * convert to themselves. See docs/LITHOLIBC-CORE.md.
 */
#ifndef LITHOLIBC_CTYPE_H
#define LITHOLIBC_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_CTYPE_H */
