/* litholibc/errno_values.h ... the LithoLibC errno value table.
 *
 * The values are LithoRoot's own C errno space. One deliberate
 * choice, flagged for the record: the numbering follows the WASI
 * errno assignments rather than the Linux ones. Piece 1 froze the
 * LithoSys error codes in WASI numbering precisely so that the
 * translation from a lithosys_err_t into a C errno is the identity
 * for every code the host surface can produce (see lithosys.h and
 * docs/LITHOSYS-FREEZE.md). Adopting the same numbering here keeps
 * that promise: no translation table exists anywhere in LithoRoot,
 * and none can drift. Code that compares errno against the symbolic
 * names, which is all conforming code, never sees a difference; the
 * numbers are visible only to code that prints them.
 *
 * Only codes with a defined meaning in this library are listed.
 * The range is dense (1..74) because the source numbering is dense;
 * strerror treats any code outside its table as unknown.
 */
#ifndef LITHOLIBC_ERRNO_VALUES_H
#define LITHOLIBC_ERRNO_VALUES_H

#define EPERM        63  /* operation not permitted */
#define ENOENT       44  /* no such file or directory */
#define ESRCH        71  /* no such process */
#define EINTR        27  /* interrupted function call */
#define EIO          29  /* input/output error */
#define ENXIO        60  /* no such device or address */
#define E2BIG         1  /* argument list too long */
#define ENOEXEC      45  /* executable format error */
#define EBADF         8  /* bad file descriptor */
#define ECHILD       12  /* no child processes */
#define EAGAIN        6  /* resource temporarily unavailable */
#define ENOMEM       48  /* out of memory */
#define EACCES        2  /* permission denied */
#define EFAULT       21  /* bad address */
#define EBUSY        10  /* device or resource busy */
#define EEXIST       20  /* file exists */
#define EXDEV        74  /* cross-device link */
#define ENODEV       43  /* no such device */
#define ENOTDIR      54  /* not a directory */
#define EISDIR       31  /* is a directory */
#define EINVAL       28  /* invalid argument */
#define ENFILE       41  /* too many open files in system */
#define EMFILE       33  /* too many open files */
#define ENOTTY       59  /* inappropriate ioctl for device */
#define ETXTBSY      73  /* text file busy */
#define EFBIG        22  /* file too large */
#define ENOSPC       51  /* no space left on device */
#define ESPIPE       70  /* illegal seek */
#define EROFS        69  /* read-only file system */
#define EMLINK       34  /* too many links */
#define EPIPE        64  /* broken pipe */
#define EDOM         18  /* math argument out of domain */
#define ERANGE       68  /* math result not representable */
#define EDEADLK      16  /* resource deadlock would occur */
#define ENAMETOOLONG 37  /* file name too long */
#define ENOLCK       46  /* no locks available */
#define ENOSYS       52  /* function not implemented */
#define ENOTEMPTY    55  /* directory not empty */
#define ELOOP        32  /* too many levels of symbolic links */
#define ENOMSG       49  /* no message of desired type */
#define EIDRM        24  /* identifier removed */
#define EILSEQ       25  /* illegal byte sequence */
#define ENOTSUP      58  /* operation not supported */
#define EOPNOTSUPP   58  /* same code, POSIX spelling */
#define EOVERFLOW    61  /* value too large for data type */
#define ECANCELED    11  /* operation canceled */
#define EOWNERDEAD   62  /* owner died */
#define ENOTRECOVERABLE 56 /* state not recoverable */
#define EADDRINUSE    3  /* address in use */
#define EADDRNOTAVAIL 4  /* address not available */
#define EAFNOSUPPORT  5  /* address family not supported */
#define EALREADY      7  /* connection already in progress */
#define EBADMSG       9  /* bad message */
#define ECONNABORTED 13  /* connection aborted */
#define ECONNREFUSED 14  /* connection refused */
#define ECONNRESET   15  /* connection reset */
#define EDESTADDRREQ 17  /* destination address required */
#define EDQUOT       19  /* quota exceeded */
#define EHOSTUNREACH 23  /* host is unreachable */
#define EINPROGRESS  26  /* operation in progress */
#define EISCONN      30  /* socket is connected */
#define EMSGSIZE     35  /* message too large */
#define EMULTIHOP    36  /* multihop attempted */
#define ENETDOWN     38  /* network is down */
#define ENETRESET    39  /* network dropped connection on reset */
#define ENETUNREACH  40  /* network is unreachable */
#define ENOBUFS      42  /* no buffer space available */
#define ENOLINK      47  /* link has been severed */
#define ENOPROTOOPT  50  /* protocol not available */
#define ENOTCONN     53  /* socket is not connected */
#define ENOTSOCK     57  /* not a socket */
#define EPROTO       65  /* protocol error */
#define EPROTONOSUPPORT 66 /* protocol not supported */
#define EPROTOTYPE   67  /* protocol wrong type for socket */
#define ETIMEDOUT    72  /* connection timed out */
#define EWOULDBLOCK   6  /* same code as EAGAIN, historical spelling */

#endif /* LITHOLIBC_ERRNO_VALUES_H */
