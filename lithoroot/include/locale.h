/* LithoRoot <locale.h>: the "C" locale only. LithoLibC formats and parses numbers in the C
 * locale always (printf/strtod never consult a locale), so setlocale accepts "C", "POSIX" and
 * "" (the environment's locale is "C" here), returns "C" for a query, and refuses anything
 * else with NULL, as the standard allows. localeconv describes the C locale. No state, so
 * nothing to lock and nothing thread-local. Header-only. */
#ifndef LITHOROOT_LOCALE_H
#define LITHOROOT_LOCALE_H
#include <limits.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL 6
struct lconv {
  char* decimal_point; char* thousands_sep; char* grouping;
  char* int_curr_symbol; char* currency_symbol; char* mon_decimal_point; char* mon_thousands_sep;
  char* mon_grouping; char* positive_sign; char* negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space,
       p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space, int_n_cs_precedes,
       int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};
static inline char* setlocale(int category, const char* locale) {
  static char c_name[] = "C";
  if (category < LC_CTYPE || category > LC_ALL) return NULL;
  if (locale == NULL) return c_name;
  if (locale[0] == '\0' || (locale[0] == 'C' && locale[1] == '\0')) return c_name;
  {
    const char* p = "POSIX";
    size_t i = 0;
    while (p[i] != '\0' && locale[i] == p[i]) ++i;
    if (p[i] == '\0' && locale[i] == '\0') return c_name;
  }
  return NULL;
}
static inline struct lconv* localeconv(void) {
  static char dot[] = ".", empty[] = "";
  static struct lconv c = {dot, empty, empty, empty, empty, empty, empty, empty, empty, empty,
                           CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
                           CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX};
  return &c;
}
/* POSIX.1-2008 per-thread locale objects, again the C locale only: newlocale hands out one
 * static object for "C"/"POSIX"/"", uselocale reports and keeps the global locale (there is
 * nothing to switch), freelocale is a no-op. */
typedef struct lithoroot_locale* locale_t;
struct lithoroot_locale { int c_only; };
#define LC_CTYPE_MASK (1 << LC_CTYPE)
#define LC_NUMERIC_MASK (1 << LC_NUMERIC)
#define LC_TIME_MASK (1 << LC_TIME)
#define LC_COLLATE_MASK (1 << LC_COLLATE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_ALL_MASK (LC_CTYPE_MASK | LC_NUMERIC_MASK | LC_TIME_MASK | LC_COLLATE_MASK | LC_MONETARY_MASK | LC_MESSAGES_MASK)
static inline locale_t lithoroot_c_locale(void) { static struct lithoroot_locale c = {1}; return &c; }
#define LC_GLOBAL_LOCALE (lithoroot_c_locale())
static inline locale_t newlocale(int mask, const char* locale, locale_t base) {
  (void)base;
  if (mask & ~LC_ALL_MASK) return (locale_t)0;
  if (locale == NULL || setlocale(LC_ALL, locale) == NULL) return (locale_t)0;
  return lithoroot_c_locale();
}
static inline locale_t uselocale(locale_t l) { (void)l; return lithoroot_c_locale(); }
static inline void freelocale(locale_t l) { (void)l; }
static inline locale_t duplocale(locale_t l) { return l; }
#ifdef __cplusplus
}
#endif
#endif
