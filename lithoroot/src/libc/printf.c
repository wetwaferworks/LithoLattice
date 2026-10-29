/* printf.c ... the printf family over one formatter core.
 *
 * One engine (format_out) walks the format string and emits
 * through a sink; three sinks cover the family: a FILE sink
 * (printf, fprintf, vprintf, vfprintf), a bounded buffer sink
 * (snprintf, vsnprintf, sprintf, vsprintf) and a counting sink
 * (the n == 0 case). The return value is the number of characters
 * that would have been written with unlimited space, or -1 on an
 * output error, per C99.
 *
 * Supported: flags - + space # 0; width and precision as numbers
 * or *; lengths hh h l ll j z t; conversions d i u x X o c s p %
 * n and the float set f F e E g G. Float conversions stand on
 * litholibc_dtoa_digits (dtoa.c): the exact decimal expansion of
 * the binary value, rounded half-to-even at the requested digit,
 * which is the correctly rounded result for every precision,
 * including %.17g round trips. %a is not implemented (the census
 * has zero call sites): an %a conversion prints literally, as do
 * other unknown conversions, and consumes no argument. The same
 * holds (review L3, documented rather than implemented: no call
 * sites, and LithoLibC has no wide-character layer) for %A, the
 * wide %lc / %ls, and the long double length L. %n IS supported,
 * so format strings must stay compile-time constants. A total
 * past INT_MAX returns -1 with errno EOVERFLOW.
 *
 * Deliberate glibc-compatible choices the differential test
 * pins down: %s of NULL prints "(null)", %p of NULL prints
 * "(nil)", 0 and space flags are ignored for c and s, and an
 * integer conversion with precision specified ignores the 0 flag.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "litholibc_internal.h"

/* ---- sinks ---------------------------------------------------------- */

typedef struct {
  int (*emit)(void* ctx, const char* data, size_t len);
  void* ctx;
  long long total; /* review L3: long is 32-bit on wasm32 */
  int error;
} sink_t;

static void sink_write(sink_t* sk, const char* data, size_t len) {
  if (len == 0 || sk->error) {
    sk->total += (long long)len;
    return;
  }
  if (sk->emit(sk->ctx, data, len) != 0) sk->error = 1;
  sk->total += (long long)len;
}
static void sink_char(sink_t* sk, char c) { sink_write(sk, &c, 1); }
static void sink_repeat(sink_t* sk, char c, long count) {
  char block[32];
  memset(block, c, sizeof block);
  while (count > 0) {
    long chunk = count > (long)sizeof block ? (long)sizeof block : count;
    sink_write(sk, block, (size_t)chunk);
    count -= chunk;
  }
}

static int file_emit(void* ctx, const char* data, size_t len) {
  return __litholibc_file_write((FILE*)ctx, data, len);
}

typedef struct {
  char* buf;
  size_t cap; /* total capacity including the NUL, 0 = count only */
  size_t used;
} buf_sink_t;

static int buf_emit(void* ctx, const char* data, size_t len) {
  buf_sink_t* b = (buf_sink_t*)ctx;
  if (b->cap > 0 && b->used < b->cap - 1) {
    size_t room = b->cap - 1 - b->used;
    size_t take = len < room ? len : room;
    memcpy(b->buf + b->used, data, take);
    b->used += take;
  }
  return 0;
}

/* ---- dtoa rounding ------------------------------------------------------
 * Round the exact digit string d[0..n) at `keep` significant
 * digits, round-half-to-even with the full tail known (so "5"
 * followed by any nonzero digit rounds up). Returns the new
 * digit count (== keep when truncation happened, or 0 when the
 * value rounds to zero at this precision: the caller then prints
 * zeros). *dec_exp moves up by one when the carry runs off the
 * front. Digits beyond the returned count are not used. */
static int round_digits(char* d, int n, int keep, int* dec_exp) {
  int i;
  if (keep >= n) return n;
  if (keep < 0) return 0;
  if (keep == 0) {
    /* The rounding digit is d[0]; the kept side is an implicit 0
     * (even), so a tie stays zero. */
    if (d[0] > '5') return -1; /* caller: rounds up to one unit */
    if (d[0] == '5') {
      for (i = 1; i < n; i++) {
        if (d[i] != '0') return -1;
      }
    }
    return 0;
  }
  {
    int up;
    if (d[keep] > '5') {
      up = 1;
    } else if (d[keep] < '5') {
      up = 0;
    } else {
      up = 0;
      for (i = keep + 1; i < n; i++) {
        if (d[i] != '0') {
          up = 1;
          break;
        }
      }
      if (!up && ((d[keep - 1] - '0') & 1)) up = 1; /* tie: to even */
    }
    if (!up) return keep;
    i = keep - 1;
    while (i >= 0 && d[i] == '9') {
      d[i] = '0';
      i--;
    }
    if (i < 0) {
      d[0] = '1';
      /* the rest are already '0' */
      (*dec_exp)++;
      return 1;
    }
    d[i]++;
    return keep;
  }
}

/* ---- float conversion ---------------------------------------------------
 * Renders one finite float conversion. dig[] and nd come from
 * litholibc_dtoa_digits (no trailing zeros, d[0] != '0'); the
 * value is 0.digits x 10^dec_exp. signch is the sign character
 * to print or 0. g_mode is set for %#-less %g: fraction digits
 * come from the significant digit count, not from a fixed
 * precision, so no trailing zeros are printed.
 *
 * Rounding happens once, here, at exactly the significant-digit
 * position the style keeps: f keeps dec_exp + fprec digits, e
 * keeps fprec + 1 digits, g keeps P digits and chooses its style
 * from the rounded exponent (per the standard). */
static void emit_float(sink_t* sk, char conv, int prec, int flags_hash,
                       int flags_zero, long width, int left, char signch,
                       const char* dig_in, int nd_in, int dec_exp_in,
                       int g_mode, int upper) {
  char dig[LITHOLIBC_DTOA_BUFSIZE + 2];
  int nd = nd_in, dec_exp = dec_exp_in;
  int style; /* 0 = f, 1 = e */
  int fprec = 0;

  memcpy(dig, dig_in, (size_t)nd);
  dig[nd] = '\0';

  if (conv == 'g') {
    int P = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
    int X;
    int m = round_digits(dig, nd, P, &dec_exp);
    if (m == -1) { /* cannot happen: P >= 1 keeps a digit */
      dig[0] = '1';
      nd = 1;
      dec_exp++;
    } else {
      nd = m;
    }
    if (g_mode) {
      /* Strip trailing zeros: they are insignificant by style. */
      while (nd > 1 && dig[nd - 1] == '0') nd--;
    }
    X = dec_exp - 1;
    if (P > X && X >= -4) {
      style = 0;
      fprec = g_mode ? (nd > dec_exp ? nd - dec_exp : 0) : (P - 1 - X);
    } else {
      style = 1;
      fprec = g_mode ? (nd - 1) : (P - 1);
    }
  } else if (conv == 'e') {
    style = 1;
    fprec = prec < 0 ? 6 : prec;
  } else {
    style = 0;
    fprec = prec < 0 ? 6 : prec;
  }

  if (style == 0) {
    if (!g_mode) {
      int keep = dec_exp + fprec;
      int m = round_digits(dig, nd, keep, &dec_exp);
      if (m == -1) {
        dig[0] = '1';
        nd = 1;
        dec_exp++;
      } else {
        nd = m;
      }
    }
    {
      int int_digits = dec_exp > 0 ? dec_exp : 1;
      long body_len = (signch ? 1 : 0) + int_digits +
                      ((fprec > 0 || flags_hash) ? 1 + fprec : 0);
      long pad = width > body_len ? width - body_len : 0;
      int i;
      if (!left && !flags_zero) sink_repeat(sk, ' ', pad);
      if (signch) sink_char(sk, signch);
      if (!left && flags_zero) sink_repeat(sk, '0', pad);
      if (dec_exp <= 0) {
        sink_char(sk, '0');
      } else {
        for (i = 0; i < dec_exp; i++) sink_char(sk, i < nd ? dig[i] : '0');
      }
      if (fprec > 0 || flags_hash) {
        sink_char(sk, '.');
        for (i = 0; i < fprec; i++) {
          int idx = dec_exp + i;
          sink_char(sk, (idx >= 0 && idx < nd) ? dig[idx] : '0');
        }
      }
      if (left) sink_repeat(sk, ' ', pad);
    }
  } else {
    if (!g_mode) {
      int keep = fprec + 1;
      int m = round_digits(dig, nd, keep, &dec_exp);
      if (m > 0) nd = m;
    }
    {
      int X = dec_exp - 1;
      char expbuf[8];
      int explen = 0;
      long body_len;
      long pad;
      int i;
      {
        int ax = X < 0 ? -X : X;
        char tmp[4];
        int tn = 0;
        if (ax == 0) tmp[tn++] = '0';
        while (ax) {
          tmp[tn++] = (char)('0' + ax % 10);
          ax /= 10;
        }
        expbuf[explen++] = upper ? 'E' : 'e';
        expbuf[explen++] = X < 0 ? '-' : '+';
        if (tn == 1) expbuf[explen++] = '0';
        while (tn > 0) expbuf[explen++] = tmp[--tn];
      }
      body_len = (signch ? 1 : 0) + 1 +
                 ((fprec > 0 || flags_hash) ? 1 + fprec : 0) + explen;
      pad = width > body_len ? width - body_len : 0;
      if (!left && !flags_zero) sink_repeat(sk, ' ', pad);
      if (signch) sink_char(sk, signch);
      if (!left && flags_zero) sink_repeat(sk, '0', pad);
      sink_char(sk, nd > 0 ? dig[0] : '0');
      if (fprec > 0 || flags_hash) {
        sink_char(sk, '.');
        for (i = 1; i <= fprec; i++) sink_char(sk, i < nd ? dig[i] : '0');
      }
      sink_write(sk, expbuf, (size_t)explen);
      if (left) sink_repeat(sk, ' ', pad);
    }
  }
}

/* ---- the formatter core ------------------------------------------------- */

enum {
  LEN_NONE,
  LEN_HH,
  LEN_H,
  LEN_L,
  LEN_LL,
  LEN_J,
  LEN_Z,
  LEN_T
};

/* The fetch helpers take va_list by ADDRESS. On targets where
 * va_list is an array type (x86-64) passing it by value happens to
 * share state; on targets where it is a scalar pointer (wasm32,
 * AArch64) the by-value copy advances privately and every fetch
 * re-reads the first argument. The by-address form is the portable
 * one (it is the pattern musl uses) and behaves identically on
 * both. Convicted by the toolchain piece probe: "%d %d %d" printed
 * the first argument three times under wasm32. */
static intmax_t fetch_signed(va_list* ap, int len) {
  switch (len) {
    case LEN_HH: return (signed char)va_arg(*ap, int);
    case LEN_H: return (short)va_arg(*ap, int);
    case LEN_L: return va_arg(*ap, long);
    case LEN_LL: return va_arg(*ap, long long);
    case LEN_J: return va_arg(*ap, intmax_t);
    case LEN_Z: return (intmax_t)va_arg(*ap, size_t);
    case LEN_T: return (intmax_t)va_arg(*ap, ptrdiff_t);
    default: return va_arg(*ap, int);
  }
}
static uintmax_t fetch_unsigned(va_list* ap, int len) {
  switch (len) {
    case LEN_HH: return (unsigned char)va_arg(*ap, unsigned int);
    case LEN_H: return (unsigned short)va_arg(*ap, unsigned int);
    case LEN_L: return va_arg(*ap, unsigned long);
    case LEN_LL: return va_arg(*ap, unsigned long long);
    case LEN_J: return va_arg(*ap, uintmax_t);
    case LEN_Z: return va_arg(*ap, size_t);
    case LEN_T: return (uintmax_t)va_arg(*ap, ptrdiff_t);
    default: return va_arg(*ap, unsigned int);
  }
}

static const char* const k_digits_lower = "0123456789abcdef";
static const char* const k_digits_upper = "0123456789ABCDEF";

static int format_out(sink_t* sk, const char* fmt, va_list ap_in) {
  /* Work on a local va_copy: the fetch helpers take the list by
   * address (see their comment), and only the address of a LOCAL
   * va_list object has the right type on both array-va_list and
   * scalar-va_list targets. Direct va_arg uses below consume this
   * same local list. */
  va_list ap;
  va_copy(ap, ap_in);
  const char* f = fmt;
  while (*f) {
    int flags_minus = 0, flags_plus = 0, flags_space = 0, flags_hash = 0,
        flags_zero = 0;
    long width = 0;
    long prec = -1;
    int len = LEN_NONE;
    char conv;
    if (*f != '%') {
      const char* start = f;
      while (*f && *f != '%') f++;
      sink_write(sk, start, (size_t)(f - start));
      continue;
    }
    f++;
    if (*f == '\0') break;
    /* flags */
    for (;;) {
      if (*f == '-') flags_minus = 1;
      else if (*f == '+') flags_plus = 1;
      else if (*f == ' ') flags_space = 1;
      else if (*f == '#') flags_hash = 1;
      else if (*f == '0') flags_zero = 1;
      else break;
      f++;
    }
    /* width */
    if (*f == '*') {
      int w = va_arg(ap, int);
      f++;
      /* Audit fix B-8: the star form takes the same sanity
       * clamp as the literal form below. Unclamped, a single
       * conversion could demand gigabytes of padding (seconds
       * of spinning, or gigabytes written to a real file), and
       * negating an INT_MIN width overflowed a 32-bit long.
       * The magnitude is clamped in int space, where every
       * value is representable, before the sign is applied. */
      if (w < 0) {
        flags_minus = 1;
        width = w < -(1 << 24) ? (1 << 24) : -w;
      } else {
        width = w > (1 << 24) ? (1 << 24) : w;
      }
    } else {
      while (isdigit((unsigned char)*f)) {
        width = width * 10 + (*f - '0');
        if (width > (1 << 24)) width = 1 << 24; /* sanity clamp */
        f++;
      }
    }
    /* precision */
    if (*f == '.') {
      f++;
      if (*f == '*') {
        int p = va_arg(ap, int);
        f++;
        /* Audit fix B-8: same clamp as the width (a negative
         * precision keeps its "omitted" meaning). */
        prec = p < 0 ? -1 : (p > (1 << 24) ? (1 << 24) : p);
      } else {
        prec = 0;
        while (isdigit((unsigned char)*f)) {
          prec = prec * 10 + (*f - '0');
          if (prec > (1 << 24)) prec = 1 << 24;
          f++;
        }
      }
    }
    /* length */
    if (f[0] == 'h' && f[1] == 'h') {
      len = LEN_HH;
      f += 2;
    } else if (f[0] == 'l' && f[1] == 'l') {
      len = LEN_LL;
      f += 2;
    } else if (*f == 'h') {
      len = LEN_H;
      f++;
    } else if (*f == 'l') {
      len = LEN_L;
      f++;
    } else if (*f == 'j') {
      len = LEN_J;
      f++;
    } else if (*f == 'z') {
      len = LEN_Z;
      f++;
    } else if (*f == 't') {
      len = LEN_T;
      f++;
    }
    conv = *f;
    if (conv) f++;

    switch (conv) {
      case '%':
        sink_char(sk, '%');
        break;
      case 'd':
      case 'i': {
        intmax_t v = fetch_signed(&ap, len);
        uintmax_t uv;
        char signch = 0;
        char numbuf[24];
        int np = 0;
        long zeros, content;
        if (v < 0) {
          signch = '-';
          uv = (uintmax_t)0 - (uintmax_t)v;
        } else {
          uv = (uintmax_t)v;
          signch = flags_plus ? '+' : (flags_space ? ' ' : 0);
        }
        if (uv == 0) {
          if (prec != 0) numbuf[np++] = '0';
        } else {
          char rev[24];
          int rp = 0;
          while (uv) {
            rev[rp++] = (char)('0' + (int)(uv % 10));
            uv /= 10;
          }
          while (rp > 0) numbuf[np++] = rev[--rp];
        }
        zeros = prec > np ? prec - np : 0;
        content = (signch ? 1 : 0) + zeros + np;
        {
          long pad = width > content ? width - content : 0;
          int zeropad = flags_zero && !flags_minus && prec < 0;
          if (!flags_minus && !zeropad) sink_repeat(sk, ' ', pad);
          if (signch) sink_char(sk, signch);
          if (!flags_minus && zeropad) sink_repeat(sk, '0', pad);
          sink_repeat(sk, '0', zeros);
          sink_write(sk, numbuf, (size_t)np);
          if (flags_minus) sink_repeat(sk, ' ', pad);
        }
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        uintmax_t uv = fetch_unsigned(&ap, len);
        unsigned base = conv == 'u' ? 10u : (conv == 'o' ? 8u : 16u);
        const char* digs = conv == 'X' ? k_digits_upper : k_digits_lower;
        char numbuf[24];
        int np = 0;
        char prefix[2];
        int prefix_len = 0;
        long zeros, content;
        if (uv == 0) {
          if (prec != 0) numbuf[np++] = '0';
        } else {
          char rev[24];
          int rp = 0;
          uintmax_t t = uv;
          while (t) {
            rev[rp++] = digs[t % base];
            t /= base;
          }
          while (rp > 0) numbuf[np++] = rev[--rp];
        }
        if (flags_hash && conv == 'o') {
          /* force a leading zero */
          if (np == 0) {
            numbuf[np++] = '0';
          } else if (numbuf[0] != '0' && prec <= np) {
            prec = np + 1;
          }
        }
        if (flags_hash && (conv == 'x' || conv == 'X') && uv != 0) {
          prefix[0] = '0';
          prefix[1] = conv;
          prefix_len = 2;
        }
        zeros = prec > np ? prec - np : 0;
        content = prefix_len + zeros + np;
        {
          long pad = width > content ? width - content : 0;
          int zeropad = flags_zero && !flags_minus && prec < 0;
          if (!flags_minus && !zeropad) sink_repeat(sk, ' ', pad);
          if (prefix_len) sink_write(sk, prefix, (size_t)prefix_len);
          if (!flags_minus && zeropad) sink_repeat(sk, '0', pad);
          sink_repeat(sk, '0', zeros);
          sink_write(sk, numbuf, (size_t)np);
          if (flags_minus) sink_repeat(sk, ' ', pad);
        }
        break;
      }
      case 'c': {
        char c = (char)va_arg(ap, int);
        long pad = width > 1 ? width - 1 : 0;
        if (!flags_minus) sink_repeat(sk, ' ', pad);
        sink_char(sk, c);
        if (flags_minus) sink_repeat(sk, ' ', pad);
        break;
      }
      case 's': {
        const char* s = va_arg(ap, const char*);
        size_t sl;
        long pad;
        if (s == NULL) s = "(null)";
        sl = strlen(s);
        if (prec >= 0 && (long)sl > prec) sl = (size_t)prec;
        pad = width > (long)sl ? width - (long)sl : 0;
        if (!flags_minus) sink_repeat(sk, ' ', pad);
        sink_write(sk, s, sl);
        if (flags_minus) sink_repeat(sk, ' ', pad);
        break;
      }
      case 'p': {
        void* ptr = va_arg(ap, void*);
        if (ptr == NULL) {
          long pad = width > 5 ? width - 5 : 0;
          if (!flags_minus) sink_repeat(sk, ' ', pad);
          sink_write(sk, "(nil)", 5);
          if (flags_minus) sink_repeat(sk, ' ', pad);
        } else {
          uintmax_t uv = (uintmax_t)(uintptr_t)ptr;
          char rev[16];
          int rp = 0;
          long content, pad;
          while (uv) {
            rev[rp++] = k_digits_lower[uv & 15];
            uv >>= 4;
          }
          content = 2 + rp;
          pad = width > content ? width - content : 0;
          if (!flags_minus) sink_repeat(sk, ' ', pad);
          sink_write(sk, "0x", 2);
          while (rp > 0) sink_char(sk, rev[--rp]);
          if (flags_minus) sink_repeat(sk, ' ', pad);
        }
        break;
      }
      case 'n': {
        long long total = sk->total;
        switch (len) {
          case LEN_HH: *va_arg(ap, signed char*) = (signed char)total; break;
          case LEN_H: *va_arg(ap, short*) = (short)total; break;
          case LEN_L: *va_arg(ap, long*) = (long)total; break;
          case LEN_LL: *va_arg(ap, long long*) = total; break;
          /* Audit fix B-10: j and t have their own store widths;
           * they used to fall into the int* default and write
           * four bytes into an eight-byte target. */
          case LEN_J: *va_arg(ap, intmax_t*) = (intmax_t)total; break;
          case LEN_T: *va_arg(ap, ptrdiff_t*) = (ptrdiff_t)total; break;
          case LEN_Z: *va_arg(ap, size_t*) = (size_t)total; break;
          default: *va_arg(ap, int*) = (int)total; break;
        }
        break;
      }
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G': {
        double v = va_arg(ap, double);
        char lconv = (conv == 'F' || conv == 'E' || conv == 'G')
                         ? (char)(conv - 'A' + 'a')
                         : conv;
        uint64_t bits;
        char signch;
        memcpy(&bits, &v, sizeof bits);
        signch = (bits >> 63) ? '-'
                              : (flags_plus ? '+' : (flags_space ? ' ' : 0));
        if (((bits >> 52) & 0x7ff) == 0x7ff) {
          /* inf or nan: no digits, no zero padding (glibc pads
           * with spaces for these). */
          const char* word;
          long pad;
          if (bits & ((UINT64_C(1) << 52) - 1)) {
            word = (conv == 'F' || conv == 'E' || conv == 'G') ? "NAN" : "nan";
          } else {
            word = (conv == 'F' || conv == 'E' || conv == 'G') ? "INF" : "inf";
          }
          pad = width > (long)(3 + (signch ? 1 : 0))
                    ? width - (3 + (signch ? 1 : 0))
                    : 0;
          if (!flags_minus) sink_repeat(sk, ' ', pad);
          if (signch) sink_char(sk, signch);
          sink_write(sk, word, 3);
          if (flags_minus) sink_repeat(sk, ' ', pad);
        } else if (v == 0.0) {
          /* Exact zero in any style: digits "0", dec_exp 1... the
           * e style wants exponent 0, which dec_exp 1 gives. */
          char dig0[2] = {'0', '\0'};
          int g_mode = (lconv == 'g' && !flags_hash) ? 1 : 0;
          emit_float(sk, lconv, (int)prec, flags_hash, flags_zero, width,
                     flags_minus, signch, dig0, 1, 1, g_mode,
                     (conv == 'E' || conv == 'G' || conv == 'F') ? 1 : 0);
        } else {
          char digs[LITHOLIBC_DTOA_BUFSIZE];
          int dec_exp, dsign;
          int nd = litholibc_dtoa_digits(v < 0 ? -v : v, digs, &dec_exp,
                                         &dsign);
          int g_mode = (lconv == 'g' && !flags_hash) ? 1 : 0;
          emit_float(sk, lconv, (int)prec, flags_hash, flags_zero, width,
                     flags_minus, signch, digs, nd, dec_exp, g_mode,
                     (conv == 'E' || conv == 'G' || conv == 'F') ? 1 : 0);
        }
        break;
      }
      default: {
        /* Unsupported conversion (including %a): print it
         * literally and consume no argument. */
        sink_char(sk, '%');
        sink_char(sk, conv);
        break;
      }
    }
  }
  {
    int result;
    if (sk->error) {
      result = -1;
    } else if (sk->total > INT_MAX) {
      /* Review L3: the count does not fit the int return; C11
       * 7.21.6.1 and POSIX answer EOVERFLOW. */
      errno = EOVERFLOW;
      result = -1;
    } else {
      result = (int)sk->total;
    }
    va_end(ap);
    return result;
  }
}

/* ---- the public family -------------------------------------------------- */

static int format_to_buffer(char* s, size_t n, const char* fmt, va_list ap) {
  buf_sink_t b;
  sink_t sk;
  int r;
  b.buf = s;
  b.cap = (s != NULL) ? n : 0;
  b.used = 0;
  sk.emit = buf_emit;
  sk.ctx = &b;
  sk.total = 0;
  sk.error = 0;
  r = format_out(&sk, fmt, ap);
  if (b.cap > 0) {
    size_t end = b.used < b.cap - 1 ? b.used : b.cap - 1;
    b.buf[end] = '\0';
  }
  return r;
}

int vsnprintf(char* s, size_t n, const char* fmt, va_list ap) {
  return format_to_buffer(s, n, fmt, ap);
}

int vsprintf(char* s, const char* fmt, va_list ap) {
  /* Unbounded by definition; the caller owns the space. */
  return format_to_buffer(s, ~(size_t)0, fmt, ap);
}

int vfprintf(FILE* file, const char* fmt, va_list ap) {
  sink_t sk;
  sk.emit = file_emit;
  sk.ctx = file;
  sk.total = 0;
  sk.error = 0;
  return format_out(&sk, fmt, ap);
}

int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int snprintf(char* s, size_t n, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(s, n, fmt, ap);
  va_end(ap);
  return r;
}

int sprintf(char* s, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsprintf(s, fmt, ap);
  va_end(ap);
  return r;
}

int fprintf(FILE* file, const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vfprintf(file, fmt, ap);
  va_end(ap);
  return r;
}

int printf(const char* fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vprintf(fmt, ap);
  va_end(ap);
  return r;
}
