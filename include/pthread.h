/* pthread.h ... LithoThread, the pthread-shaped API of LithoRoot.
 *
 * LithoThread is a deliberately cut pthread surface implemented
 * in-module over LithoWeave (thread spawn / join / exit and the
 * TLS destructor registry) and LithoSys (the frozen host clock;
 * atomic wait / notify are instructions, not imports, on the wasm
 * target). It exists so engine and library code written against
 * the standard thread API links against our own implementation
 * once the vendor runtime is gone. docs/LITHOTHREAD.md carries
 * the design note: the construction of each primitive, the cut
 * list with its reasons, and the honest remaining risks.
 *
 * Scope, in one breath: thread create / join / detach / self /
 * equal / exit with a minimal attribute object (detach state,
 * stack size); mutexes in the normal, errorcheck and recursive
 * flavors; condition variables with monotonic or realtime timed
 * waits; reader-preference rwlocks; once; and TLS keys whose
 * destructors ride LithoWeave's exit registry. NOT here, by
 * design: cancellation, signals, robust mutexes, priority and
 * scheduling attributes, barriers, spinlocks as a public type,
 * fork handlers. Each exclusion is argued in the design note.
 *
 * Error convention is POSIX pthread's own: functions return the
 * error number directly and never write errno.
 *
 * Types: pthread_t is the weave handle itself (a plain value:
 * control pointer plus tid). The synchronization types are small
 * complete structs, because C code allocates them statically and
 * initializes them with the PTHREAD_*_INITIALIZER macros; their
 * layouts are therefore ABI, are fixed at 16 bytes (4 for
 * pthread_once_t), and are documented field by field below.
 * Internals stay private by convention: nothing outside
 * LithoThread reads or writes a field directly.
 *
 * Header shape notes: strict C99 declarations, extern "C"
 * guarded, no restrict qualifiers (the matrix's C++ compilers
 * reject plain restrict in headers; Pieces 1 to 3 set the
 * precedent). <time.h> is included for struct timespec and
 * clockid_t: on native builds today that is the host header;
 * LithoRoot grows its own time.h with the stdio / stdlib / math
 * piece, and it must keep these shapes when it lands.
 */
#ifndef LITHOLIBC_PTHREAD_H
#define LITHOLIBC_PTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* <time.h> only promises struct timespec from C11 and the
 * clock ids under POSIX feature selection, so a strict-C99
 * consumer of this header may have neither. Supply the missing
 * pieces, guarded to never collide with the host's own in
 * either include order (the timespec guard macro below is the
 * one the common host headers themselves use; the clockid_t
 * typedef travels with the CLOCK_* constants, which every
 * implementation in our matrix exposes or withholds together).
 * The future LithoLibC time.h retires this block by providing
 * all of it natively. */
#if !defined(_STRUCT_TIMESPEC) && defined(__STDC_VERSION__) && \
    (__STDC_VERSION__ < 201112L)
#define _STRUCT_TIMESPEC 1
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
#endif
#ifndef CLOCK_REALTIME
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#endif

#include "lithoweave.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LITHOTHREAD_VERSION 1

/* ---- constants ------------------------------------------------------ */

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_KEYS_MAX              128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

/* ---- types ------------------------------------------------------------
 * Synchronization structs: every field is a 32-bit word so the
 * sizes below hold on 32-bit and 64-bit targets alike.
 *
 * pthread_mutex_t:
 *   state ... 0 unlocked, 1 locked (no waiters marked),
 *             2 locked (waiters may be sleeping on state).
 *             The futex-style three-state word the wait protocol
 *             is built on; see the design note.
 *   type .... PTHREAD_MUTEX_* flavor. 0 is NORMAL, which is what
 *             makes the all-zero static initializer a valid
 *             normal mutex.
 *   owner ... weave tid of the holder; maintained only for the
 *             errorcheck and recursive flavors.
 *   count ... recursion depth for the recursive flavor.
 *
 * pthread_cond_t:
 *   seq ....... sequence word: every signal and broadcast bumps
 *               it (atomically) and then notifies its address.
 *               Waiters snapshot it under the user mutex and
 *               sleep on it with a value-checked wait, which is
 *               the whole no-missed-wakeup mechanism.
 *   waiters ... count of threads inside a wait (signal skips the
 *               notify when it reads 0; nothing else trusts it).
 *   clockid ... clock timed waits take their deadline on
 *               (CLOCK_REALTIME or CLOCK_MONOTONIC).
 *   reserved .. destroy poison; 0 in every live cond.
 *
 * pthread_rwlock_t:
 *   lock ...... bit 31: a writer holds it; bits 0..29: reader
 *               count. Reader-preference by design (note has it).
 *   rseq ...... sequence word readers sleep on (bumped by a
 *               releasing writer).
 *   wseq ...... sequence word writers sleep on (bumped by a
 *               releasing writer and by the last departing reader).
 *   reserved .. destroy poison; 0 in every live rwlock.
 */

typedef lw_thread_t pthread_t;

typedef struct {
  uint32_t detachstate; /* PTHREAD_CREATE_* */
  uint32_t reserved;
  size_t stacksize;     /* 0: weave / host default */
} pthread_attr_t;

typedef struct {
  uint32_t state;
  uint32_t type;
  uint32_t owner;
  uint32_t count;
} pthread_mutex_t;

typedef struct {
  int32_t type; /* PTHREAD_MUTEX_* */
} pthread_mutexattr_t;

typedef struct {
  uint32_t seq;
  uint32_t waiters;
  int32_t clockid;
  uint32_t reserved;
} pthread_cond_t;

typedef struct {
  int32_t clockid;
  int32_t pshared;
} pthread_condattr_t;

typedef struct {
  uint32_t lock;
  uint32_t rseq;
  uint32_t wseq;
  uint32_t reserved;
} pthread_rwlock_t;

typedef struct {
  int32_t pshared;
} pthread_rwlockattr_t;

typedef struct {
  uint32_t state; /* 0 fresh, 1 running, 2 done */
} pthread_once_t;

typedef uint32_t pthread_key_t;

/* ---- static initializers -------------------------------------------- */

#define PTHREAD_MUTEX_INITIALIZER  { 0u, 0u, 0u, 0u }
#define PTHREAD_MUTEX_RECURSIVE_INITIALIZER \
  { 0u, (uint32_t)PTHREAD_MUTEX_RECURSIVE, 0u, 0u }
#define PTHREAD_MUTEX_ERRORCHECK_INITIALIZER \
  { 0u, (uint32_t)PTHREAD_MUTEX_ERRORCHECK, 0u, 0u }
#define PTHREAD_COND_INITIALIZER   { 0u, 0u, (int32_t)CLOCK_REALTIME, 0u }
#define PTHREAD_RWLOCK_INITIALIZER { 0u, 0u, 0u, 0u }
#define PTHREAD_ONCE_INIT          { 0u }

/* ---- threads ---------------------------------------------------------- */

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg);
int pthread_join(pthread_t thread, void** retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
void pthread_exit(void* retval);

/* ---- thread attributes -------------------------------------------------- */

int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setdetachstate(pthread_attr_t* attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detachstate);
int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* stacksize);

/* ---- mutexes -------------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutex_timedlock(pthread_mutex_t* mutex,
                            const struct timespec* abstime);

int pthread_mutexattr_init(pthread_mutexattr_t* attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t* attr);
int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t* attr, int pshared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t* attr, int* pshared);

/* ---- condition variables ---------------------------------------------------- */

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);

int pthread_condattr_init(pthread_condattr_t* attr);
int pthread_condattr_destroy(pthread_condattr_t* attr);
int pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock_id);
int pthread_condattr_getclock(const pthread_condattr_t* attr,
                              clockid_t* clock_id);
int pthread_condattr_setpshared(pthread_condattr_t* attr, int pshared);
int pthread_condattr_getpshared(const pthread_condattr_t* attr, int* pshared);

/* ---- read / write locks ------------------------------------------------------- */

int pthread_rwlock_init(pthread_rwlock_t* rwlock,
                        const pthread_rwlockattr_t* attr);
int pthread_rwlock_destroy(pthread_rwlock_t* rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_timedrdlock(pthread_rwlock_t* rwlock,
                               const struct timespec* abstime);
int pthread_rwlock_timedwrlock(pthread_rwlock_t* rwlock,
                               const struct timespec* abstime);

int pthread_rwlockattr_init(pthread_rwlockattr_t* attr);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t* attr);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t* attr, int pshared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t* attr,
                                  int* pshared);

/* ---- once ------------------------------------------------------------------------ */

int pthread_once(pthread_once_t* once_control, void (*init_routine)(void));

/* ---- thread-specific data --------------------------------------------------------- */

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void* value);
void* pthread_getspecific(pthread_key_t key);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_PTHREAD_H */
