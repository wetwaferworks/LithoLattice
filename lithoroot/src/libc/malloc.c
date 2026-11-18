/* malloc.c ... the LithoLibC allocation family.
 *
 * Thin wrappers over WaferHeap's default heap layer (wh_libc_*),
 * doing the one job WaferHeap deliberately leaves to the libc
 * boundary: translating a NULL failure into errno. WaferHeap's own
 * failure semantics carry over unchanged: realloc that fails leaves
 * the original block untouched, calloc refuses an overflowing
 * product, and free of NULL or of a pointer foreign to the heap is
 * a no-op. Successful calls never write errno.
 *
 * strdup and strndup live here rather than in string.c because
 * they are allocation functions that happen to copy strings.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "waferheap.h"
#include "lithoheap.h"

/* Once crt1 installs the lock-free shared heap (lithothread_heap.c),
 * every allocation goes through it. Weak, so builds without
 * LithoThread (the native suites) keep the single default heap. */
#pragma weak lh_installed
#pragma weak lh_malloc
#pragma weak lh_free
#pragma weak lh_realloc
#pragma weak lh_calloc
#pragma weak lh_aligned_alloc
#pragma weak lh_usable_size
#define LR_SHARED() (lh_installed != 0 && lh_installed())

void* malloc(size_t size) {
  void* p = (LR_SHARED() ? lh_malloc(size) : wh_libc_malloc(size));
  if (p == NULL) errno = ENOMEM;
  return p;
}

void free(void* ptr) {
  if (LR_SHARED()) lh_free(ptr);
  else wh_libc_free(ptr);
}

void* realloc(void* ptr, size_t size) {
  void* p = (LR_SHARED() ? lh_realloc(ptr, size) : wh_libc_realloc(ptr, size));
  /* wh_libc_realloc with size 0 frees and returns NULL by design;
   * that NULL is a success, not an out-of-memory failure. */
  if (p == NULL && size != 0) errno = ENOMEM;
  return p;
}

void* calloc(size_t count, size_t size) {
  void* p = (LR_SHARED() ? lh_calloc(count, size) : wh_libc_calloc(count, size));
  if (p == NULL) errno = ENOMEM;
  return p;
}

void* aligned_alloc(size_t alignment, size_t size) {
  void* p;
  /* WaferHeap requires a power of two at least sizeof(void*). Say
   * EINVAL for anything else instead of letting it fail vaguely.
   * The C11 rule that size be a multiple of alignment is not
   * enforced: no caller is served by failing such a request, and
   * the wider libc world does not enforce it either. */
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
    errno = EINVAL;
    return NULL;
  }
  p = (LR_SHARED() ? lh_aligned_alloc(alignment, size) : wh_libc_aligned_alloc(alignment, size));
  if (p == NULL) errno = ENOMEM;
  return p;
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
  void* p;
  /* The POSIX contract: the error number is the return value,
   * errno is not touched, and *memptr is written only on success.
   * Alignment must be a power of two multiple of sizeof(void*). */
  if (alignment < sizeof(void*) || (alignment % sizeof(void*)) != 0 ||
      (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  p = (LR_SHARED() ? lh_aligned_alloc(alignment, size) : wh_libc_aligned_alloc(alignment, size));
  if (p == NULL) return ENOMEM;
  *memptr = p;
  return 0;
}

size_t malloc_usable_size(const void* ptr) {
  wh_heap_t* heap;
  if (LR_SHARED()) return lh_usable_size(ptr);
  heap = wh_default_heap();
  if (heap == NULL || ptr == NULL) return 0;
  return wh_malloc_usable_size(heap, ptr);
}

char* strdup(const char* s) {
  const size_t len = strlen(s) + 1;
  char* copy = (char*)malloc(len);
  if (copy == NULL) return NULL; /* malloc already set ENOMEM */
  memcpy(copy, s, len);
  return copy;
}

char* strndup(const char* s, size_t n) {
  const size_t len = strnlen(s, n);
  char* copy = (char*)malloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, s, len);
  copy[len] = '\0';
  return copy;
}
