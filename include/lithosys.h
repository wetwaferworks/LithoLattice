/* lithosys.h ... the LithoSys interface freeze, version 1.
 *
 * LithoSys is the system interface of LithoRoot: the complete, minimal
 * set of host services that the wasm build of the engine may require
 * from its host (the web page layer today; a native LithoSys backend
 * library on other platforms) once the retired vendor runtime is gone.
 * LithoLibC, WaferHeap, LithoWeave / LithoThread and LithoAbi are
 * implemented in-module on top of this surface and nothing else.
 *
 * Contract rules (frozen with this header; see
 * docs/LITHOSYS-FREEZE.md for the full freeze note):
 *
 *  - Import binding: on wasm these symbols are module imports under
 *    module "env", with exactly the names declared here. On native
 *    platforms the same declarations are ordinary functions provided
 *    by the platform's LithoSys backend (for example LithoSys-Win32).
 *    The header carries no import attributes and no platform headers,
 *    so it compiles unchanged on the wasm target, on Windows / MSVC
 *    and on native Linux. Types are fixed-width ints only.
 *
 *  - Errors: a function that can fail returns a lithosys_err_t code
 *    (LITHOSYS_OK on success). A LithoSys call never traps, aborts or
 *    silently pretends to succeed on an unsupported operation: it
 *    returns LITHOSYS_ENOSYS (or a more specific code) and the caller
 *    decides. This is the deliberate fail-soft contract; see the
 *    freeze note.
 *
 *  - Threads: every function here may be called from any thread
 *    unless its comment says otherwise. No function blocks
 *    indefinitely. Waiting and signalling between threads are done
 *    with the wasm atomic wait / notify instructions on shared
 *    memory, which are instructions, not imports, and so do not
 *    appear in this surface.
 *
 *  - Memory: linear memory may only be grown through
 *    lithosys_memory_grow, never with the memory.grow instruction
 *    directly, so the host can enforce its cap and refresh its views.
 */
#ifndef LITHOSYS_H
#define LITHOSYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LITHOSYS_VERSION 1

/* One wasm page, in bytes. The wasm standard fixes this at 64 KiB. */
#define LITHOSYS_PAGE_SIZE 65536u

/* ---- error codes ----------------------------------------------------
 * LITHOSYS_OK is 0. The nonzero values deliberately match the WASI
 * errno numbering for the codes listed, so LithoLibC's translation
 * into the C errno space is a mechanical pass-through. Only the codes
 * the frozen surface can actually produce are defined here. */
typedef int32_t lithosys_err_t;
#define LITHOSYS_OK      0
#define LITHOSYS_EAGAIN  6   /* would block; retry later */
#define LITHOSYS_EBADF   8   /* no such open fd */
#define LITHOSYS_EFAULT  21  /* bad pointer argument */
#define LITHOSYS_EINTR   27  /* interrupted before completion */
#define LITHOSYS_EINVAL  28  /* invalid argument */
#define LITHOSYS_EIO     29  /* host I/O error */
#define LITHOSYS_ENOENT  44  /* no such file or entry */
#define LITHOSYS_ENOMEM  48  /* growth or allocation refused */
#define LITHOSYS_ENOSPC  51  /* host storage exhausted */
#define LITHOSYS_ENOSYS  52  /* not supported by this host: fail-soft */
#define LITHOSYS_ENOTSUP 58  /* supported surface, unsupported request */
#define LITHOSYS_EPERM   63  /* host refused on policy */
#define LITHOSYS_ERANGE  68  /* result out of representable range */
#define LITHOSYS_ESPIPE  70  /* fd is not seekable */

/* ---- clocks ---------------------------------------------------------
 * Passed to lithosys_clock_now_ns. MONOTONIC never runs backwards and
 * is the engine's timebase. REALTIME is wall time (seconds since the
 * Unix epoch), for timestamps and calendar work in LithoLibC. */
#define LITHOSYS_CLOCK_MONOTONIC 0u
#define LITHOSYS_CLOCK_REALTIME  1u

/* ---- reserved file descriptors --------------------------------------
 * The only fds a host must provide. Any other fd number only exists if
 * a future surface addition (see the freeze note's open questions)
 * creates it. */
#define LITHOSYS_FD_STDIN  0u
#define LITHOSYS_FD_STDOUT 1u
#define LITHOSYS_FD_STDERR 2u

/* ---- seek positions (match the C stdio SEEK_* values) --------------- */
#define LITHOSYS_SEEK_SET 0u
#define LITHOSYS_SEEK_CUR 1u
#define LITHOSYS_SEEK_END 2u

/* ---- scatter / gather element ---------------------------------------
 * One buffer segment for lithosys_fd_read / lithosys_fd_write. On the
 * wasm32 target this is a (u32 pointer, u32 length) pair in linear
 * memory, which is how the page layer reads it; on native platforms
 * both sides of the call are our own code and the native pointer
 * width applies. */
typedef struct lithosys_iov {
  const void* buf;
  uint32_t len;
} lithosys_iov_t;

/* Portability shim for the two calls that never return. */
#if defined(_MSC_VER)
#define LITHOSYS_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define LITHOSYS_NORETURN __attribute__((__noreturn__))
#elif defined(__cplusplus)
#define LITHOSYS_NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define LITHOSYS_NORETURN _Noreturn
#else
#define LITHOSYS_NORETURN
#endif

/* ==== memory ======================================================== */

/* Grow the shared linear memory by delta_pages pages, through the
 * host. The host enforces its own maximum (which may be lower than
 * the memory's declared maximum) and refreshes any cached views of
 * memory before returning. On success returns LITHOSYS_OK and, when
 * out_old_pages is not NULL, stores the page count from before the
 * grow. On refusal returns LITHOSYS_ENOMEM and memory is unchanged.
 * Callable from any thread; concurrent grows are serialized by the
 * host. Replaces the vendor's emscripten_resize_heap; the heap cap
 * the vendor exposed through emscripten_get_heap_max is expressed
 * here as grow refusal, and the current size is available in-module
 * through the memory.size instruction, so neither needs an import. */
lithosys_err_t lithosys_memory_grow(uint32_t delta_pages,
                                    uint32_t* out_old_pages);

/* ==== time ========================================================== */

/* Read a clock into out_ns as nanoseconds: since an unspecified fixed
 * origin for LITHOSYS_CLOCK_MONOTONIC, since the Unix epoch for
 * LITHOSYS_CLOCK_REALTIME. Hosts quantize to their real resolution
 * (the web host's monotonic clock is performance.now based and may
 * be coarsened by the browser; that is accepted and documented).
 * Returns LITHOSYS_OK, or LITHOSYS_EINVAL for an unknown clock id.
 * Replaces the vendor's clock_time_get, emscripten_get_now and
 * emscripten_date_now with one call. */
lithosys_err_t lithosys_clock_now_ns(uint32_t clock_id, uint64_t* out_ns);

/* ==== threads (the LithoWeave spawn contract) ======================= */

/* Spawn a host thread (a Web Worker on the web host) running this
 * module. start_arg is the address, in shared linear memory, of a
 * LithoWeave control block prepared by the caller; its first word is
 * the thread's tid cell. The host starts the worker, which enters
 * the module through the exported lithosys_thread_start function
 * (frozen name; declared by LithoWeave, not here) with start_arg as
 * its argument.
 *
 * Returns the new thread id (a positive value) on success, or the
 * negation of a lithosys_err_t code on failure (for example
 * -LITHOSYS_EAGAIN when the host refuses more threads).
 *
 * Exit protocol (frozen): when the worker's start function returns,
 * the host stores 0 into the tid cell and performs an atomic notify
 * on the cell's address, then retires the worker. Joining is done
 * in-module by LithoThread, which waits on the tid cell with the
 * atomic wait instruction. Thread-local cleanup is LithoThread's
 * in-module business and needs no host call. This one import
 * replaces the vendor's whole thread JS family:
 * __pthread_create_js, _emscripten_thread_cleanup,
 * _emscripten_thread_set_strongref, _emscripten_init_main_thread_js,
 * _emscripten_thread_mailbox_await,
 * _emscripten_receive_on_main_thread_js and
 * _emscripten_notify_mailbox_postmessage (the mailbox family existed
 * to proxy main-thread services to workers; the frozen surface is
 * any-thread by contract, so no proxy is needed). */
int32_t lithosys_thread_spawn(uint32_t start_arg);

/* ==== file descriptors ============================================== */

/* Gather-write iovs to an open fd. On success returns LITHOSYS_OK
 * and stores the byte count in out_written (when not NULL). Writing
 * to LITHOSYS_FD_STDOUT / LITHOSYS_FD_STDERR reaches the host console
 * or log and is best-effort. Any fd the host does not have fails soft
 * with LITHOSYS_EBADF; a host with no writable fds beyond the console
 * says so with LITHOSYS_ENOSYS. Replaces the vendor's fd_write, which
 * carried all of the engine's logging and stdio output. */
lithosys_err_t lithosys_fd_write(uint32_t fd, const lithosys_iov_t* iovs,
                                 uint32_t iovs_len, uint32_t* out_written);

/* Scatter-read from an open fd into iovs; the read counterpart of
 * lithosys_fd_write, replacing the vendor's fd_read. Frozen stdin
 * behavior: reads on LITHOSYS_FD_STDIN succeed and report 0 bytes
 * (permanent end-of-file) on hosts with no input stream, so stdio
 * sees an ordinary EOF instead of an error. */
lithosys_err_t lithosys_fd_read(uint32_t fd, const lithosys_iov_t* iovs,
                                uint32_t iovs_len, uint32_t* out_read);

/* Close an open fd. Closing one of the reserved fds (0, 1, 2) is a
 * no-op success: the console cannot be closed out from under the
 * engine. Any other unknown fd returns LITHOSYS_EBADF. Replaces the
 * vendor's fd_close. Never traps. */
lithosys_err_t lithosys_fd_close(uint32_t fd);

/* Reposition an open, seekable fd; stores the new absolute position
 * in out_pos (when not NULL). offset is interpreted per whence
 * (LITHOSYS_SEEK_*). On the web host every fd is non-seekable, so
 * this fails soft with LITHOSYS_ESPIPE; native backends implement it
 * for real files. Replaces the vendor's fd_seek. */
lithosys_err_t lithosys_fd_seek(uint32_t fd, int64_t offset, uint32_t whence,
                                uint64_t* out_pos);

/* Flush an open fd to its backing store. The reserved console fds
 * flush trivially (success). Any other fd on a host without durable
 * files fails soft with LITHOSYS_ENOSYS. Replaces the vendor's
 * fd_sync. */
lithosys_err_t lithosys_fd_sync(uint32_t fd);

/* ==== the read-only volume (ruling addendum) ======================== */

/* Open a file by path inside the host's read-only volume and store
 * the resulting fd in out_fd. Added under the ruling addendum to
 * the freeze (JD, 2026-10-28 17:48, recorded in
 * docs/LITHOSYS-FREEZE.md): the host registers the fetched pack
 * blob as a read-only volume; this call is the volume's open, and
 * the frozen lithosys_fd_read, lithosys_fd_seek and
 * lithosys_fd_close above serve the fd it returns. The pack format
 * is unchanged and LithoLibC's stdio keeps its usual call shape:
 * fopen routes a read through here first.
 *
 * The volume is read-only by definition: a volume fd accepts reads
 * and seeks only, and LithoLibC refuses write modes against it with
 * EROFS before any write is attempted. The fd is seekable (on the
 * web host it addresses a byte range inside the registered blob),
 * unlike the reserved console fds.
 *
 * path is a NUL-terminated volume path, treated as an opaque key
 * by the host except for its own lookup rules; LithoLibC passes
 * the fopen path through unmodified. On success returns
 * LITHOSYS_OK. A path the volume does not hold returns
 * LITHOSYS_ENOENT; a host with no volume registered returns
 * LITHOSYS_ENOSYS. Never traps. */
lithosys_err_t lithosys_volume_open(const char* path, uint32_t* out_fd);

/* ==== last-resort console =========================================== */

/* Write len bytes from buf to the host console or log, directly,
 * bypassing the fd table and all LithoLibC state. This is the sink
 * for assert failures, aborts and early boot, where libc state may
 * not exist or may be what just broke. It cannot fail observably:
 * the return is void, the host does its best, and the call is safe
 * from any thread and from inside lithosys_abort. On the web host
 * this is the same destination fd 1 writes reach, reached through
 * the host's own serialized path. */
void lithosys_console_write(const char* buf, uint32_t len);

/* ==== random ======================================================== */

/* Fill buf with len bytes of host randomness (on the web host,
 * crypto.getRandomValues). Returns LITHOSYS_OK, or LITHOSYS_ENOSYS
 * on a host with no randomness source, in which case the buffer is
 * left untouched and the caller must not treat it as entropy.
 * Replaces the vendor's random_get. */
lithosys_err_t lithosys_random_fill(void* buf, uint32_t len);

/* ==== host query ==================================================== */

/* The number of logical cores the host offers (on the web host,
 * navigator.hardwareConcurrency). Always succeeds: a host that
 * cannot know returns 1. Replaces the vendor's
 * emscripten_num_logical_cores. */
uint32_t lithosys_cpu_count(void);

/* ==== process control =============================================== */

/* End the module instance with the given exit code. The host stops
 * the frame loop (whose registration lives in the lk platform layer,
 * not here) and records the code; control never returns to the
 * caller. Replaces the vendor's exit and
 * emscripten_exit_with_live_runtime: the "live runtime" distinction
 * was about the vendor's own callback bookkeeping, and the frozen
 * contract simply has the host own teardown. */
LITHOSYS_NORETURN void lithosys_exit(int32_t code);

/* Abort the module instance immediately, after whatever the caller
 * already wrote through lithosys_console_write. No cleanup handlers
 * run. Replaces the vendor's _abort_js. LithoLibC's __assert_fail
 * equivalent is an in-module function that reports through
 * lithosys_console_write and ends here, so assertion needs no import
 * of its own. */
LITHOSYS_NORETURN void lithosys_abort(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef LITHOSYS_NORETURN

#endif /* LITHOSYS_H */
