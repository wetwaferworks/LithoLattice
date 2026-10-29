/* scanf.c ... the scanf family over one input engine.
 *
 * The census story for scanf is small but real: sscanf parses
 * "%d,%d"-shaped configuration lines and float triples, fscanf
 * has exactly one call site, and the family rounds out the
 * formatted-input surface the headers must offer. The engine
 * (vscan_core) works over an abstract source: a FILE (through
 * fgetc / ungetc) or a string cursor.
 *
 * Conversion set: d i o u x X, e E f g G (through strtod on the
 * collected token), s, c, [set], p (read as a hex integer into a
 * void**), n, and %% matching. Assignment suppression, field
 * widths, and the hh h l ll j z t lengths are supported for the
 * integer conversions; floats take l for double and default to
 * float. Overflow in the integer conversions saturates (the
 * standard leaves it undefined; saturation matches the strtol
 * family's behavior in this library and cannot wrap silently).
 *
 * Return contract: the number of assignments, or EOF when an
 * input error (end of file) occurs before the first conversion
 * completes. A matching failure returns the assignments so far.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void* ctx;
  int (*getch)(void* ctx);
  int (*ungetch)(void* ctx, int c);
  long consumed; /* net characters taken from the source */
  int hit_eof;   /* an input error (EOF) was observed */
} scan_src_t;

static int src_get(scan_src_t* s) {
  int c = s->getch(s->ctx);
  if (c == EOF) {
    s->hit_eof = 1;
  } else {
    s->consumed++;
  }
  return c;
}
static void src_unget(scan_src_t* s, int c) {
  if (c == EOF) return;
  if (s->ungetch(s->ctx, c) != EOF) s->consumed--;
}

/* FILE source */
static int file_getch(void* ctx) { return fgetc((FILE*)ctx); }
static int file_ungetch(void* ctx, int c) { return ungetc(c, (FILE*)ctx); }

/* string source */
typedef struct {
  const char* p;
} str_src_t;
static int str_getch(void* ctx) {
  str_src_t* s = (str_src_t*)ctx;
  if (*s->p == '\0') return EOF;
  return (unsigned char)*s->p++;
}
static int str_ungetch(void* ctx, int c) {
  str_src_t* s = (str_src_t*)ctx;
  (void)c;
  s->p--;
  return c;
}

/* ---- helpers -------------------------------------------------------------- */

static void skip_ws(scan_src_t* s) {
  int c;
  do {
    c = src_get(s);
  } while (c != EOF && isspace(c));
  if (c != EOF) src_unget(s, c);
}

static int digit_val(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

enum {
  SL_NONE,
  SL_HH,
  SL_H,
  SL_L,
  SL_LL,
  SL_J,
  SL_Z,
  SL_T
};

/* va_list is taken by ADDRESS here for the same reason as in
 * printf.c's fetch helpers: on scalar-va_list targets (wasm32,
 * AArch64) a by-value copy advances privately and every store
 * would write through the FIRST pointer argument again. */
static void store_signed(va_list* ap, int len, intmax_t v) {
  switch (len) {
    case SL_HH: *va_arg(*ap, signed char*) = (signed char)v; break;
    case SL_H: *va_arg(*ap, short*) = (short)v; break;
    case SL_L: *va_arg(*ap, long*) = (long)v; break;
    case SL_LL: *va_arg(*ap, long long*) = (long long)v; break;
    case SL_J: *va_arg(*ap, intmax_t*) = v; break;
    case SL_Z: *va_arg(*ap, size_t*) = (size_t)v; break;
    case SL_T: *va_arg(*ap, ptrdiff_t*) = (ptrdiff_t)v; break;
    default: *va_arg(*ap, int*) = (int)v; break;
  }
}
static void store_unsigned(va_list* ap, int len, uintmax_t v) {
  switch (len) {
    case SL_HH: *va_arg(*ap, unsigned char*) = (unsigned char)v; break;
    case SL_H: *va_arg(*ap, unsigned short*) = (unsigned short)v; break;
    case SL_L: *va_arg(*ap, unsigned long*) = (unsigned long)v; break;
    case SL_LL: *va_arg(*ap, unsigned long long*) = v; break;
    case SL_J: *va_arg(*ap, uintmax_t*) = v; break;
    case SL_Z: *va_arg(*ap, size_t*) = (size_t)v; break;
    case SL_T: *va_arg(*ap, ptrdiff_t*) = (ptrdiff_t)v; break;
    default: *va_arg(*ap, unsigned int*) = (unsigned int)v; break;
  }
}

/* Scan an integer. Returns 1 on success, 0 on matching failure,
 * -1 when EOF arrived before any digit. local_width is the
 * remaining field width (negative = unlimited). */
static int scan_int(scan_src_t* s, int base, int is_signed, long width,
                    uintmax_t* out_mag, int* out_neg) {
  long w = width;
  int neg = 0, any = 0;
  uintmax_t acc = 0;
  int c;
  skip_ws(s);
  c = src_get(s);
  if (c == EOF) return -1;
  if (c == '+' || c == '-') {
    neg = (c == '-');
    if (w > 0) w--;
    c = src_get(s);
    if (c == EOF) return -1;
  }
  if (base == 0) {
    if (c == '0') {
      /* Peek for x. */
      int c2;
      if (w == 1 && width >= 0) {
        base = 8;
      } else {
        c2 = src_get(s);
        if ((c2 == 'x' || c2 == 'X')) {
          /* 0x counts only if a hex digit follows: one more peek. */
          int c3 = src_get(s);
          if (c3 != EOF && digit_val(c3) >= 0 && digit_val(c3) < 16) {
            base = 16;
            acc = 0;
            any = 1; /* digits follow for sure */
            src_unget(s, c3);
            if (w > 0) {
              w -= 2; /* the 0 and the x consumed width */
              if (w < 0) w = 0;
            }
            c = src_get(s);
          } else {
            if (c3 != EOF) src_unget(s, c3);
            src_unget(s, c2);
            base = 8;
          }
        } else {
          if (c2 != EOF) src_unget(s, c2);
          base = 8;
        }
      }
    } else {
      base = 10;
    }
  } else if (base == 16 && c == '0') {
    /* Optional 0x consumption is handled by the digit loop
     * naturally ('0' is a hex digit); glibc consumes the x too
     * when a digit follows, but the value is identical either
     * way and the leftover 'x' would end the field... so do the
     * peek here as well. */
    int c2 = src_get(s);
    if (c2 == 'x' || c2 == 'X') {
      int c3 = src_get(s);
      if (c3 != EOF && digit_val(c3) >= 0 && digit_val(c3) < 16) {
        if (w > 0) {
          w -= 2;
          if (w < 0) w = 0;
        }
        c = c3;
      } else {
        if (c3 != EOF) src_unget(s, c3);
        src_unget(s, c2);
      }
    } else {
      if (c2 != EOF) src_unget(s, c2);
    }
  }
  (void)is_signed;
  while (c != EOF) {
    int d = digit_val(c);
    if (d < 0 || d >= base) break;
    if (width >= 0 && w <= 0) break;
    any = 1;
    /* accumulate with saturation */
    if (acc > (UINTMAX_MAX - (uintmax_t)d) / (uintmax_t)base) {
      acc = UINTMAX_MAX;
    } else {
      acc = acc * (uintmax_t)base + (uintmax_t)d;
    }
    if (w > 0) w--;
    c = src_get(s);
  }
  if (c != EOF) src_unget(s, c);
  if (!any) return s->hit_eof ? -1 : 0;
  *out_mag = acc;
  *out_neg = neg;
  return 1;
}

/* Scan a float token and convert. Returns like scan_int. */
static int scan_float(scan_src_t* s, long width, double* out) {
  char tok[1200];
  size_t tn = 0;
  long w = width;
  int c, any = 0;
  int state = 0; /* 0 start, 1 int digits, 2 frac, 3 exp mark, 4 exp */
  skip_ws(s);
  c = src_get(s);
  if (c == EOF) return -1;
  if (c == '+' || c == '-') {
    if (tn < sizeof tok - 1) tok[tn++] = (char)c;
    if (w > 0) w--;
    c = src_get(s);
  }
  /* inf / nan words */
  if (c != EOF && (c == 'i' || c == 'I' || c == 'n' || c == 'N')) {
    char word[16];
    size_t wn = 0;
    size_t matched = 0;
    while (c != EOF && isalpha(c) && wn < sizeof word - 1) {
      word[wn++] = (char)tolower(c);
      c = src_get(s);
    }
    word[wn] = '\0';
    /* Audit fix B-5: match the standard's subject sequence
     * exactly ("inf", "infinity", "nan", "nan(chars)") instead
     * of consuming the whole alphabetic run and testing its
     * first three letters. The old shape ate "info" whole for
     * input "info42", leaving the following %d nothing sane to
     * read. Whatever the run over-read past the matched word
     * goes back below. */
    if (strncmp(word, "inf", 3) == 0) {
      matched = 3;
      if (wn >= 8 && strncmp(word + 3, "inity", 5) == 0) matched = 8;
    } else if (strncmp(word, "nan", 3) == 0) {
      matched = 3;
    }
    if (matched > 0) {
      size_t i;
      for (i = 0; i < matched && tn < sizeof tok - 1; i++)
        tok[tn++] = word[i];
      if (matched == 3 && word[0] == 'n' && c == '(') {
        /* nan(chars): commit the parenthesized sequence only
         * when it closes cleanly over n-characters; otherwise
         * everything read for the attempt goes back and the
         * token is the bare "nan" with '(' as its terminator. */
        char par[16];
        size_t pn = 0;
        int ok = 1, closed = 0;
        int c2 = src_get(s);
        while (c2 != EOF) {
          if (c2 == ')') {
            closed = 1;
            break;
          }
          if (!(isalnum(c2) || c2 == '_') || pn >= sizeof par) {
            ok = 0;
            break;
          }
          par[pn++] = (char)c2;
          c2 = src_get(s);
        }
        if (ok && closed) {
          if (tn < sizeof tok - 1) tok[tn++] = '(';
          for (i = 0; i < pn && tn < sizeof tok - 1; i++)
            tok[tn++] = par[i];
          if (tn < sizeof tok - 1) tok[tn++] = ')';
          c = src_get(s); /* the terminator after ')' */
        } else {
          if (c2 != EOF) src_unget(s, c2);
          while (pn > 0) src_unget(s, par[--pn]);
          c = '(';
        }
      }
      tok[tn] = '\0';
      /* Push back everything past the matched word: the
       * terminator first, then the over-read tail in reverse
       * (pushback is a stack: last pushed, first read back). */
      if (c != EOF) src_unget(s, c);
      for (i = wn; i > matched; i--) src_unget(s, word[i - 1]);
      *out = strtod(tok, NULL);
      return 1;
    }
    /* Not a float word: a matching failure. Characters already
     * consumed are not all recoverable; that is acceptable for
     * an input that was never a number. */
    if (c != EOF) src_unget(s, c);
    return 0;
  }
  while (c != EOF) {
    int take = 0;
    if (width >= 0 && w <= 0 && tn > 0) break;
    if (isdigit(c)) {
      take = 1;
      if (state == 0) state = 1;
      else if (state == 3 || state == 4) state = 4;
    } else if (c == '.' && (state == 0 || state == 1)) {
      take = 1;
      state = 2;
    } else if ((c == 'e' || c == 'E') && (state == 1 || state == 2)) {
      take = 1;
      state = 3;
    } else if ((c == '+' || c == '-') && state == 3) {
      take = 1;
      state = 4;
    } else {
      break;
    }
    if (take) {
      any = any || (isdigit(c) != 0);
      if (tn < sizeof tok - 1) tok[tn++] = (char)c;
      if (w > 0) w--;
      c = src_get(s);
    }
  }
  /* The terminating character goes back FIRST (pushback is a
   * stack: last read, first restored). */
  if (c != EOF) src_unget(s, c);
  /* A dangling exponent marker is not part of the subject
   * sequence: state 3 means an 'e' (and maybe a sign) was taken
   * with no exponent digits behind it. Give those characters
   * back to the source, after the terminator above. */
  if (state == 3) {
    while (tn > 0) {
      char last = tok[tn - 1];
      if (last == 'e' || last == 'E') {
        src_unget(s, tok[--tn]);
        break;
      }
      if ((last == '+' || last == '-') && tn > 1 &&
          (tok[tn - 2] == 'e' || tok[tn - 2] == 'E')) {
        src_unget(s, tok[--tn]);
        continue;
      }
      break;
    }
  }
  if (!any) return s->hit_eof ? -1 : 0;
  tok[tn] = '\0';
  *out = strtod(tok, NULL);
  return 1;
}

/* ---- the driver -------------------------------------------------------------- */

static int vscan_core(scan_src_t* s, const char* fmt, va_list ap_in) {
  /* Local va_copy, as in printf.c's format_out: the store helpers
   * take the list by address, which needs a local object. */
  va_list ap;
  va_copy(ap, ap_in);
  int assigned = 0;
  const char* f = fmt;
  while (*f) {
    if (isspace((unsigned char)*f)) {
      skip_ws(s);
      f++;
      continue;
    }
    if (*f != '%') {
      int c = src_get(s);
      if (c == EOF) return assigned == 0 ? EOF : assigned;
      if (c != *f) {
        src_unget(s, c);
        return assigned;
      }
      f++;
      continue;
    }
    f++; /* the % */
    {
      int suppress = 0;
      long width = -1;
      int len = SL_NONE;
      char conv;
      if (*f == '*') {
        suppress = 1;
        f++;
      }
      if (isdigit((unsigned char)*f)) {
        width = 0;
        while (isdigit((unsigned char)*f)) {
          /* Audit fix B-4: saturate as the accumulation goes,
           * the same 1<<24 ceiling printf clamps to. The raw
           * accumulation overflowed the long (undefined
           * behavior), and a wrapped width could land negative,
           * which this engine reads as its unlimited sentinel:
           * a bounded %s or %[ silently became unbounded. The
           * clamp keeps every intermediate value small, so the
           * multiply itself can no longer overflow either. */
          width = width * 10 + (*f - '0');
          if (width > (1L << 24)) width = (1L << 24);
          f++;
        }
      }
      if (f[0] == 'h' && f[1] == 'h') {
        len = SL_HH;
        f += 2;
      } else if (f[0] == 'l' && f[1] == 'l') {
        len = SL_LL;
        f += 2;
      } else if (*f == 'h') {
        len = SL_H;
        f++;
      } else if (*f == 'l') {
        len = SL_L;
        f++;
      } else if (*f == 'j') {
        len = SL_J;
        f++;
      } else if (*f == 'z') {
        len = SL_Z;
        f++;
      } else if (*f == 't') {
        len = SL_T;
        f++;
      } else if (*f == 'L') {
        len = SL_L; /* long double is out of the surface; read as
                       double precision into a double */
        f++;
      }
      conv = *f;
      if (conv == '\0') break;
      f++;

      switch (conv) {
        case '%': {
          int c = src_get(s);
          if (c == EOF) return assigned == 0 ? EOF : assigned;
          if (c != '%') {
            src_unget(s, c);
            return assigned;
          }
          break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
          int base = conv == 'i'   ? 0
                     : conv == 'o' ? 8
                     : (conv == 'x' || conv == 'X' || conv == 'p') ? 16
                                                                   : 10;
          uintmax_t mag = 0;
          int neg = 0;
          int r = scan_int(s, base, conv == 'd' || conv == 'i', width, &mag,
                           &neg);
          if (r <= 0) {
            if (r < 0 && assigned == 0) return EOF;
            return assigned;
          }
          if (!suppress) {
            if (conv == 'p') {
              uintmax_t v = neg ? (uintmax_t)0 - mag : mag;
              *va_arg(ap, void**) = (void*)(uintptr_t)v;
            } else if (conv == 'd' || conv == 'i') {
              intmax_t v;
              if (neg) {
                v = (mag == (uintmax_t)INTMAX_MAX + 1) ? INTMAX_MIN
                                                       : -(intmax_t)mag;
              } else {
                v = mag > (uintmax_t)INTMAX_MAX ? INTMAX_MAX : (intmax_t)mag;
              }
              store_signed(&ap, len, v);
            } else {
              uintmax_t v = neg ? (uintmax_t)0 - mag : mag;
              store_unsigned(&ap, len, v);
            }
            assigned++;
          }
          break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
          double dv = 0;
          int r = scan_float(s, width, &dv);
          if (r <= 0) {
            if (r < 0 && assigned == 0) return EOF;
            return assigned;
          }
          if (!suppress) {
            if (len == SL_L || len == SL_LL) {
              *va_arg(ap, double*) = dv;
            } else {
              *va_arg(ap, float*) = (float)dv;
            }
            assigned++;
          }
          break;
        }
        case 's': {
          char* outp = NULL;
          long w = width;
          int c, any = 0;
          skip_ws(s);
          if (!suppress) outp = va_arg(ap, char*);
          c = src_get(s);
          if (c == EOF) return assigned == 0 ? EOF : assigned;
          while (c != EOF && !isspace(c)) {
            if (width >= 0 && w <= 0) break;
            if (outp) *outp++ = (char)c;
            any = 1;
            if (w > 0) w--;
            c = src_get(s);
          }
          if (c != EOF) src_unget(s, c);
          if (!any) return assigned;
          if (outp) *outp = '\0';
          if (!suppress) assigned++;
          break;
        }
        case 'c': {
          char* outp = NULL;
          long want = width < 0 ? 1 : width;
          long got = 0;
          if (!suppress) outp = va_arg(ap, char*);
          while (got < want) {
            int c = src_get(s);
            if (c == EOF) break;
            if (outp) outp[got] = (char)c;
            got++;
          }
          if (got == 0) return assigned == 0 && s->hit_eof ? EOF : assigned;
          if (got < want) {
            /* Short read at EOF: the partial field still counts
             * as a conversion per the common behavior. */
          }
          if (!suppress) assigned++;
          break;
        }
        case '[': {
          int negate = 0;
          bool set[256];
          char* outp = NULL;
          long w = width;
          int c, any = 0;
          memset(set, 0, sizeof set);
          if (*f == '^') {
            negate = 1;
            f++;
          }
          if (*f == ']') {
            set[(unsigned char)']'] = true;
            f++;
          }
          {
            int prev = -1;
            while (*f && *f != ']') {
              if (*f == '-' && prev >= 0 && f[1] != ']' && f[1] != '\0') {
                int hi = (unsigned char)f[1];
                int k;
                for (k = prev; k <= hi; k++) set[k] = true;
                prev = -1;
                f += 2;
              } else {
                set[(unsigned char)*f] = true;
                prev = (unsigned char)*f;
                f++;
              }
            }
            if (*f == ']') f++;
          }
          if (!suppress) outp = va_arg(ap, char*);
          c = src_get(s);
          if (c == EOF) return assigned == 0 ? EOF : assigned;
          while (c != EOF) {
            int in = set[(unsigned char)c] ? 1 : 0;
            if (negate) in = !in;
            if (!in) break;
            if (width >= 0 && w <= 0) break;
            if (outp) *outp++ = (char)c;
            any = 1;
            if (w > 0) w--;
            c = src_get(s);
          }
          if (c != EOF) src_unget(s, c);
          if (!any) return assigned;
          if (outp) *outp = '\0';
          if (!suppress) assigned++;
          break;
        }
        case 'n': {
          if (!suppress) {
            store_signed(&ap, len, (intmax_t)s->consumed);
          }
          break;
        }
        default:
          /* Unknown conversion: stop, returning what we have. */
          va_end(ap);
          return assigned;
      }
    }
  }
  va_end(ap);
  return assigned;
}

/* ---- the public family ------------------------------------------------------- */

int vfscanf(FILE* file, const char* fmt, va_list ap) {
  scan_src_t s;
  s.ctx = file;
  s.getch = file_getch;
  s.ungetch = file_ungetch;
  s.consumed = 0;
  s.hit_eof = 0;
  return vscan_core(&s, fmt, ap);
}

int vscanf(const char* fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int vsscanf(const char* str, const char* fmt, va_list ap) {
  str_src_t ss;
  scan_src_t s;
  ss.p = str;
  s.ctx = &ss;
  s.getch = str_getch;
  s.ungetch = str_ungetch;
  s.consumed = 0;
  s.hit_eof = 0;
  return vscan_core(&s, fmt, ap);
}

int fscanf(FILE* file, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vfscanf(file, fmt, ap);
  va_end(ap);
  return r;
}

int scanf(const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vscanf(fmt, ap);
  va_end(ap);
  return r;
}

int sscanf(const char* str, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsscanf(str, fmt, ap);
  va_end(ap);
  return r;
}
