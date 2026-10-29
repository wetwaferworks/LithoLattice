/* time.c ... LithoLibC time over lithosys_clock_now_ns.
 *
 * time() reads the REALTIME clock; clock() reads the MONOTONIC
 * clock in microseconds (CLOCKS_PER_SEC is 1000000 in our
 * time.h). Civil conversions are UTC, per the LithoSys freeze
 * (the timezone question was left open and no offset call
 * exists): localtime is gmtime, exactly. The civil <-> days
 * conversions are the well-known era-based algorithms (Howard
 * Hinnant's public-domain formulation, re-derived here): the
 * calendar is treated as 400-year eras of 146097 days, which
 * keeps every intermediate small and exact in 64-bit arithmetic.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lithosys.h"

time_t time(time_t* out) {
  uint64_t ns = 0;
  time_t t;
  (void)lithosys_clock_now_ns(LITHOSYS_CLOCK_REALTIME, &ns);
  t = (time_t)(ns / UINT64_C(1000000000));
  if (out) *out = t;
  return t;
}

clock_t clock(void) {
  uint64_t ns = 0;
  (void)lithosys_clock_now_ns(LITHOSYS_CLOCK_MONOTONIC, &ns);
  return (clock_t)(ns / UINT64_C(1000));
}

double difftime(time_t end, time_t start) {
  return (double)(end - start);
}

/* ---- civil conversions ------------------------------------------------ */

/* Days since 1970-01-01 from a civil date (proleptic Gregorian).
 * The era arithmetic works for any year representable in the
 * inputs; tm_year is int, so the useful span is enormous. */
static long long days_from_civil(long long y, unsigned m, unsigned d) {
  long long era;
  unsigned yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);                   /* [0, 399] */
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      /* [0, 146096] */
  return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long* y, unsigned* m,
                            unsigned* d) {
  long long era;
  unsigned doe, yoe, doy, mp;
  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);               /* [0, 146096] */
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  *y = (long long)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);    /* [0, 365] */
  mp = (5 * doy + 2) / 153;                          /* [0, 11] */
  *d = doy - (153 * mp + 2) / 5 + 1;                 /* [1, 31] */
  *m = mp + (mp < 10 ? 3 : -9);                      /* [1, 12] */
  *y += (*m <= 2);
}

struct tm* gmtime_r(const time_t* tp, struct tm* out) {
  long long t = (long long)*tp;
  long long days = t / 86400;
  long long secs = t % 86400;
  long long y;
  unsigned mo, d;
  if (secs < 0) {
    secs += 86400;
    days -= 1;
  }
  civil_from_days(days, &y, &mo, &d);
  out->tm_year = (int)(y - 1900);
  out->tm_mon = (int)mo - 1;
  out->tm_mday = (int)d;
  out->tm_hour = (int)(secs / 3600);
  out->tm_min = (int)((secs % 3600) / 60);
  out->tm_sec = (int)(secs % 60);
  /* 1970-01-01 was a Thursday (wday 4). */
  out->tm_wday = (int)(((days % 7) + 4 + 7) % 7);
  out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
  out->tm_isdst = 0;
  return out;
}

struct tm* localtime_r(const time_t* tp, struct tm* out) {
  /* UTC under the freeze: local time is GMT. */
  return gmtime_r(tp, out);
}

static struct tm g_tm_slot;

struct tm* gmtime(const time_t* tp) { return gmtime_r(tp, &g_tm_slot); }
struct tm* localtime(const time_t* tp) { return localtime_r(tp, &g_tm_slot); }

time_t mktime(struct tm* tm) {
  /* Normalize out-of-range fields by going through the day
   * count: total seconds from the (possibly unnormalized)
   * fields, then a gmtime round trip rewrites the struct in
   * normalized form. Months are folded into [1,12] first and
   * mday is treated as an offset from that month's first day,
   * so mday 0, negative days, hour 25 and friends all land where
   * the standard expects. */
  {
    long long y = (long long)tm->tm_year + 1900;
    long long mon = (long long)tm->tm_mon; /* 0-based, any value */
    long long ny = y + (mon >= 0 ? mon / 12 : -((-mon + 11) / 12));
    unsigned nm = (unsigned)(mon - (ny - y) * 12) + 1;   /* [1,12] */
    long long first = days_from_civil(ny, nm, 1);
    long long days = first + (long long)tm->tm_mday - 1;
    long long secs;
    time_t result;
    secs = days * 86400 + (long long)tm->tm_hour * 3600 +
           (long long)tm->tm_min * 60 + (long long)tm->tm_sec;
    result = (time_t)secs;
    gmtime_r(&result, tm);
    return result;
  }
}

/* ---- strftime ----------------------------------------------------------- */

static const char* const k_wday_full[] = {"Sunday",   "Monday", "Tuesday",
                                          "Wednesday", "Thursday", "Friday",
                                          "Saturday"};
static const char* const k_wday_abbr[] = {"Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat"};
static const char* const k_mon_full[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
static const char* const k_mon_abbr[] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};

struct fmt_out {
  char* s;
  size_t max;
  size_t len;
  int overflow;
};

static void out_char(struct fmt_out* o, char c) {
  if (o->overflow) return;
  if (o->len + 1 >= o->max) {
    o->overflow = 1;
    return;
  }
  o->s[o->len++] = c;
}
/* Table index normalization (audit fix B-9): C's % keeps the
 * dividend's sign, so a directly assigned broken-down time with
 * a negative tm_mon or tm_wday indexed BEFORE the name tables
 * (an out-of-bounds read of a static array). Normalize into
 * [0, n) first; in-range values are unaffected. */
static int idx_mod(int v, int n) {
  int r = v % n;
  return r < 0 ? r + n : r;
}

static void out_str(struct fmt_out* o, const char* s) {
  while (*s) out_char(o, *s++);
}
static void out_num(struct fmt_out* o, long long v, int width, char pad) {
  char tmp[24];
  int n = 0, neg = 0;
  unsigned long long uv;
  if (v < 0) {
    neg = 1;
    uv = (unsigned long long)(-(v + 1)) + 1;
  } else {
    uv = (unsigned long long)v;
  }
  if (uv == 0) tmp[n++] = '0';
  while (uv) {
    tmp[n++] = (char)('0' + (int)(uv % 10));
    uv /= 10;
  }
  if (neg) out_char(o, '-');
  while (n + (neg ? 1 : 0) < width) {
    out_char(o, pad);
    width--;
  }
  while (n > 0) out_char(o, tmp[--n]);
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm) {
  struct fmt_out o;
  const char* f = format;
  o.s = s;
  o.max = max;
  o.len = 0;
  o.overflow = 0;
  if (max == 0) return 0;
  while (*f) {
    char c;
    if (*f != '%') {
      out_char(&o, *f++);
      continue;
    }
    f++;
    c = *f;
    if (c == '\0') break;
    f++;
    switch (c) {
      case 'A': out_str(&o, k_wday_full[idx_mod(tm->tm_wday, 7)]); break;
      case 'a': out_str(&o, k_wday_abbr[idx_mod(tm->tm_wday, 7)]); break;
      case 'B': out_str(&o, k_mon_full[idx_mod(tm->tm_mon, 12)]); break;
      case 'b':
      case 'h': out_str(&o, k_mon_abbr[idx_mod(tm->tm_mon, 12)]); break;
      case 'C': out_num(&o, (tm->tm_year + 1900) / 100, 2, '0'); break;
      case 'c': {
        /* The C-locale default: "Tue Oct 28 18:02:11 2026". */
        char sub[64];
        size_t n = strftime(sub, sizeof sub, "%a %b %e %H:%M:%S %Y", tm);
        sub[n] = '\0';
        out_str(&o, sub);
        break;
      }
      case 'D': {
        char sub[32];
        size_t n = strftime(sub, sizeof sub, "%m/%d/%y", tm);
        sub[n] = '\0';
        out_str(&o, sub);
        break;
      }
      case 'd': out_num(&o, tm->tm_mday, 2, '0'); break;
      case 'e': out_num(&o, tm->tm_mday, 2, ' '); break;
      case 'F': {
        char sub[32];
        size_t n = strftime(sub, sizeof sub, "%Y-%m-%d", tm);
        sub[n] = '\0';
        out_str(&o, sub);
        break;
      }
      case 'H': out_num(&o, tm->tm_hour, 2, '0'); break;
      case 'I': {
        int h = tm->tm_hour % 12;
        out_num(&o, h == 0 ? 12 : h, 2, '0');
        break;
      }
      case 'j': out_num(&o, tm->tm_yday + 1, 3, '0'); break;
      case 'M': out_num(&o, tm->tm_min, 2, '0'); break;
      case 'm': out_num(&o, tm->tm_mon + 1, 2, '0'); break;
      case 'n': out_char(&o, '\n'); break;
      case 'P': out_str(&o, tm->tm_hour < 12 ? "am" : "pm"); break;
      case 'p': out_str(&o, tm->tm_hour < 12 ? "AM" : "PM"); break;
      case 'R': {
        char sub[16];
        size_t n = strftime(sub, sizeof sub, "%H:%M", tm);
        sub[n] = '\0';
        out_str(&o, sub);
        break;
      }
      case 'r': {
        char sub[32];
        size_t n = strftime(sub, sizeof sub, "%I:%M:%S %p", tm);
        sub[n] = '\0';
        out_str(&o, sub);
        break;
      }
      case 'S': out_num(&o, tm->tm_sec, 2, '0'); break;
      case 'T': {
        char sub[16];
        size_t n = strftime(sub, sizeof sub, "%H:%M:%S", tm);
        sub[n] = '\0';
        out_str(&o, sub);
        break;
      }
      case 't': out_char(&o, '\t'); break;
      case 'u': out_num(&o, tm->tm_wday == 0 ? 7 : tm->tm_wday, 1, '0'); break;
      case 'w': out_num(&o, tm->tm_wday, 1, '0'); break;
      case 'Y': out_num(&o, tm->tm_year + 1900, 4, '0'); break;
      case 'y': out_num(&o, (tm->tm_year + 1900) % 100, 2, '0'); break;
      case 'Z': out_str(&o, "UTC"); break;
      case 'z': out_str(&o, "+0000"); break;
      case '%': out_char(&o, '%'); break;
      default:
        /* Unknown specifier: emit it literally. */
        out_char(&o, '%');
        out_char(&o, c);
        break;
    }
  }
  if (o.overflow) return 0;
  s[o.len] = '\0';
  return o.len;
}

char* asctime(const struct tm* tm) {
  static char buf[27];
  /* "Tue Oct 28 18:02:11 2026\n" ... the standard's shape. */
  size_t n = strftime(buf, 26, "%a %b %e %H:%M:%S %Y", tm);
  buf[n] = '\n';
  buf[n + 1] = '\0';
  return buf;
}

char* ctime(const time_t* t) { return asctime(localtime(t)); }
