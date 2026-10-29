/* litholibc_internal.h ... declarations shared between the files
 * of the stdio / stdlib / math piece. NOT a public header. */
#ifndef LITHOLIBC_INTERNAL_H
#define LITHOLIBC_INTERNAL_H

/* ---- dtoa (dtoa.c) --------------------------------------------------
 * Exact decimal expansion of a finite, nonzero double:
 *   v = (sign ? -1 : +1) * 0.d[0] d[1] ... d[n-1] * 10^dec_exp
 * with d[0] != '0' and no trailing zeros. The expansion is
 * complete: every digit of the exact binary value is present
 * (a double's exact decimal expansion never exceeds 767
 * significant digits, and the buffer requirement below allows
 * 800). digits must point to at least LITHOLIBC_DTOA_BUFSIZE
 * bytes. Returns n. */
#define LITHOLIBC_DTOA_BUFSIZE 800
int litholibc_dtoa_digits(double v, char* digits, int* dec_exp, int* sign);

/* ---- stdio (stdio.c) -------------------------------------------------
 * Flush every open stream's output buffer. Called by stdlib's
 * exit() after the atexit handlers, per the C standard's exit
 * sequence. Best effort: a failing stream does not stop the
 * others. */
void __litholibc_stdio_flush_all(void);

/* Write len bytes through a FILE's buffer (the normal output
 * path), returning 0 on success and -1 on error with the stream's
 * error flag set. printf.c's FILE sink and the puts family ride
 * this so buffering policy lives in exactly one place. */
struct litholibc_file;
int __litholibc_file_write(struct litholibc_file* f, const char* data,
                           size_t len);

/* ---- environment (stdlib.c) ------------------------------------------
 * Inject one "NAME=value" pair into getenv's in-module table.
 * Clearly internal: the frozen contract (LithoSys freeze, open
 * question 3) is an EMPTY environment on the web host; this hook
 * exists for native back-ends and tests to seed the table, and it
 * stores the pointer without copying, so the string must outlive
 * the process (string literals and static storage qualify). */
void __litholibc_env_inject(const char* name_eq_value);

#endif /* LITHOLIBC_INTERNAL_H */
