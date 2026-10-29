/* time.h ... the LithoLibC time header.
 *
 * The census set plus a sane completion set, over
 * lithosys_clock_now_ns. Types are our own: time_t and clock_t
 * are 64-bit counts (seconds and microseconds respectively),
 * which keeps the wasm32 and native shapes identical and puts
 * the year 2038 problem out of scope.
 *
 * Civil time is UTC, per the LithoSys freeze (the timezone
 * question was left open there and no offset call exists):
 * localtime and gmtime are the same conversion, and strftime
 * reports %Z as "UTC" and %z as "+0000".
 *
 * clock_gettime and nanosleep are NOT declared: the census has
 * no call sites for them, and LithoThread's wait machinery rides
 * LithoSys directly rather than through a POSIX-shaped header.
 */
#ifndef LITHOLIBC_TIME_H
#define LITHOLIBC_TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long long time_t;
typedef long long clock_t;

/* clock() counts microseconds of monotonic time. One million per
 * second, documented here because CLOCKS_PER_SEC is a contract
 * other code divides by. */
#define CLOCKS_PER_SEC 1000000

struct tm {
  int tm_sec;   /* seconds after the minute: 0..60 */
  int tm_min;   /* minutes after the hour: 0..59 */
  int tm_hour;  /* hours since midnight: 0..23 */
  int tm_mday;  /* day of the month: 1..31 */
  int tm_mon;   /* months since January: 0..11 */
  int tm_year;  /* years since 1900 */
  int tm_wday;  /* days since Sunday: 0..6 */
  int tm_yday;  /* days since January 1: 0..365 */
  int tm_isdst; /* daylight saving flag: always 0 under UTC */
};

time_t time(time_t* out);
clock_t clock(void);
double difftime(time_t end, time_t start);

struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime_r(const time_t* t, struct tm* out);
time_t mktime(struct tm* tm);

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);
char* asctime(const struct tm* tm);
char* ctime(const time_t* t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_TIME_H */
