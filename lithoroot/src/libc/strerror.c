/* strerror.c ... LithoLibC error strings.
 *
 * The message table is indexed directly by the error number: our
 * errno space (see litholibc/errno_values.h) is dense over 0..74,
 * so no search and no mapping layer exists. The wording is ours,
 * kept short and plain. A code outside the table formats into one
 * shared static buffer as "Unknown error N"; that buffer is the
 * one piece of strerror state, and it is why strerror is not yet
 * thread-safe for unknown codes. Known codes return pointers to
 * string literals and are safe from any thread. strerror never
 * writes errno.
 */

#include <stddef.h>
#include <string.h>

static const char* const lk_error_messages[75] = {
  /*  0 */ "Success",
  /*  1 */ "Argument list too long",
  /*  2 */ "Permission denied",
  /*  3 */ "Address already in use",
  /*  4 */ "Address not available",
  /*  5 */ "Address family not supported",
  /*  6 */ "Resource temporarily unavailable",
  /*  7 */ "Connection already in progress",
  /*  8 */ "Bad file descriptor",
  /*  9 */ "Bad message",
  /* 10 */ "Resource busy",
  /* 11 */ "Operation canceled",
  /* 12 */ "No child processes",
  /* 13 */ "Connection aborted",
  /* 14 */ "Connection refused",
  /* 15 */ "Connection reset by peer",
  /* 16 */ "Deadlock would occur",
  /* 17 */ "Destination address required",
  /* 18 */ "Math argument outside domain",
  /* 19 */ "Disk quota exceeded",
  /* 20 */ "File already exists",
  /* 21 */ "Bad address",
  /* 22 */ "File too large",
  /* 23 */ "Host unreachable",
  /* 24 */ "Identifier removed",
  /* 25 */ "Illegal byte sequence",
  /* 26 */ "Operation in progress",
  /* 27 */ "Interrupted system call",
  /* 28 */ "Invalid argument",
  /* 29 */ "Input/output error",
  /* 30 */ "Socket is connected",
  /* 31 */ "Is a directory",
  /* 32 */ "Too many levels of symbolic links",
  /* 33 */ "Too many open files",
  /* 34 */ "Too many links",
  /* 35 */ "Message too large",
  /* 36 */ "Multihop attempted",
  /* 37 */ "File name too long",
  /* 38 */ "Network is down",
  /* 39 */ "Network reset the connection",
  /* 40 */ "Network unreachable",
  /* 41 */ "Too many open files in system",
  /* 42 */ "No buffer space available",
  /* 43 */ "No such device",
  /* 44 */ "No such file or directory",
  /* 45 */ "Exec format error",
  /* 46 */ "No locks available",
  /* 47 */ "Link has been severed",
  /* 48 */ "Out of memory",
  /* 49 */ "No message of the desired type",
  /* 50 */ "Protocol not available",
  /* 51 */ "No space left on device",
  /* 52 */ "Function not implemented",
  /* 53 */ "Socket is not connected",
  /* 54 */ "Not a directory",
  /* 55 */ "Directory not empty",
  /* 56 */ "State not recoverable",
  /* 57 */ "Not a socket",
  /* 58 */ "Operation not supported",
  /* 59 */ "Inappropriate operation for device",
  /* 60 */ "No such device or address",
  /* 61 */ "Value too large for data type",
  /* 62 */ "Owner died",
  /* 63 */ "Operation not permitted",
  /* 64 */ "Broken pipe",
  /* 65 */ "Protocol error",
  /* 66 */ "Protocol not supported",
  /* 67 */ "Protocol wrong type for socket",
  /* 68 */ "Math result not representable",
  /* 69 */ "Read-only file system",
  /* 70 */ "Illegal seek",
  /* 71 */ "No such process",
  /* 72 */ "Connection timed out",
  /* 73 */ "Text file busy",
  /* 74 */ "Cross-device link",
};

char* strerror(int errnum) {
  if (errnum >= 0 &&
      errnum < (int)(sizeof lk_error_messages / sizeof lk_error_messages[0]) &&
      lk_error_messages[errnum] != NULL) {
    return (char*)lk_error_messages[errnum];
  }
  /* Unknown code: build "Unknown error N" by hand (no stdio in
   * this library yet, and strerror must not depend on it anyway).
   * The magnitude is accumulated unsigned so INT_MIN is safe. */
  {
    static char buf[32];
    char digits[12];
    size_t len = 0;
    size_t ndigits = 0;
    unsigned int mag;
    const char* prefix = "Unknown error ";
    size_t i;
    for (i = 0; prefix[i] != '\0'; ++i) buf[len++] = prefix[i];
    if (errnum < 0) {
      buf[len++] = '-';
      mag = 0u - (unsigned int)errnum;
    } else {
      mag = (unsigned int)errnum;
    }
    do {
      digits[ndigits++] = (char)('0' + (int)(mag % 10u));
      mag /= 10u;
    } while (mag != 0);
    while (ndigits > 0) buf[len++] = digits[--ndigits];
    buf[len] = '\0';
    return buf;
  }
}
