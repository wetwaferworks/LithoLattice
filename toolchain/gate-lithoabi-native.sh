#!/bin/bash
# LithoAbi native gate, plain arrangement, per tests/lithoabi_test.cpp.
set -e
cd ~/workspace/lk/lithoroot
mkdir -p /tmp/abigate
NAMES="pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock
pthread_mutex_trylock pthread_mutex_unlock pthread_mutexattr_init
pthread_mutexattr_destroy pthread_mutexattr_settype
pthread_mutexattr_gettype pthread_mutexattr_setpshared
pthread_mutexattr_getpshared pthread_cond_init
pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait
pthread_cond_signal pthread_cond_broadcast
pthread_condattr_init pthread_condattr_destroy
pthread_condattr_setclock pthread_condattr_getclock
pthread_condattr_setpshared pthread_condattr_getpshared
pthread_rwlock_init pthread_rwlock_destroy
pthread_rwlock_rdlock pthread_rwlock_tryrdlock
pthread_rwlock_wrlock pthread_rwlock_trywrlock
pthread_rwlock_unlock pthread_rwlock_timedrdlock
pthread_rwlock_timedwrlock pthread_rwlockattr_init
pthread_rwlockattr_destroy pthread_rwlockattr_setpshared
pthread_rwlockattr_getpshared pthread_once pthread_key_create
pthread_key_delete pthread_setspecific pthread_getspecific
pthread_create pthread_join pthread_detach pthread_self
pthread_equal pthread_exit pthread_attr_init pthread_attr_destroy
pthread_attr_setdetachstate pthread_attr_getdetachstate
pthread_attr_setstacksize pthread_attr_getstacksize"
REN=""; for n in $NAMES; do REN="$REN -D$n=lt_$n"; done

gcc -std=c99 -pedantic -Wall -Wextra -I include -c src/waferheap.c -o /tmp/abigate/wh.o
gcc -std=c99 -pedantic -Wall -Wextra -I include -c src/lithoweave.c -o /tmp/abigate/lw.o
for f in memory string strings ctype malloc strerror; do
  gcc -std=c99 -pedantic -Wall -Wextra -I include -c src/libc/$f.c -o /tmp/abigate/libc_$f.o
done
for f in lithothread lithothread_errno lithothread_heap; do
  gcc -std=c99 -pedantic -Wall -Wextra -I include $REN -c src/$f.c -o /tmp/abigate/$f.o
done
gcc -std=c99 -Wall -Wextra -c src/lithoweave_native.c -o /tmp/abigate/lwn.o
gcc -std=c99 -Wall -Wextra -c src/lithothread_native.c -o /tmp/abigate/ltn.o
for f in eh_runtime std_exceptions stubs typeinfo personality backend_native_scaffold; do
  g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include -I src/lithoabi -c src/lithoabi/$f.cc -o /tmp/abigate/$f.o
done
for f in exception atexit guard; do
  g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include -I src/lithoabi $REN -c src/lithoabi/$f.cc -o /tmp/abigate/$f.o
done
g++ -std=c++17 -fno-exceptions -Wall -Wextra -I include -I src/lithoabi $REN \
    tests/lithoabi_test.cpp /tmp/abigate/*.o \
    -o /tmp/abigate/lithoabi_test -nostdlib++ -pthread
/tmp/abigate/lithoabi_test
