/* lithosys_check.c ... host compile check for the LithoSys freeze.
 *
 * Takes the address of every function declared in lithosys.h, each
 * into a variable of its exact declared type, so a signature drift in
 * the header (or a missing declaration) fails this compile. This is a
 * compile check only: the definitions belong to the host, so nothing
 * here is linked or run.
 *
 * Build (from the lithoroot directory):
 *   cc -std=c99 -Wall -Wextra -Werror -I include -c tests/lithosys_check.c
 */
#include "lithosys.h"

/* One typed pointer per frozen function. */
static lithosys_err_t (*p_memory_grow)(uint32_t, uint32_t*) =
    lithosys_memory_grow;
static lithosys_err_t (*p_clock_now_ns)(uint32_t, uint64_t*) =
    lithosys_clock_now_ns;
static int32_t (*p_thread_spawn)(uint32_t) = lithosys_thread_spawn;
static lithosys_err_t (*p_fd_write)(uint32_t, const lithosys_iov_t*,
                                    uint32_t, uint32_t*) = lithosys_fd_write;
static lithosys_err_t (*p_fd_read)(uint32_t, const lithosys_iov_t*,
                                   uint32_t, uint32_t*) = lithosys_fd_read;
static lithosys_err_t (*p_fd_close)(uint32_t) = lithosys_fd_close;
static lithosys_err_t (*p_fd_seek)(uint32_t, int64_t, uint32_t,
                                   uint64_t*) = lithosys_fd_seek;
static lithosys_err_t (*p_fd_sync)(uint32_t) = lithosys_fd_sync;
static void (*p_console_write)(const char*, uint32_t) =
    lithosys_console_write;
static lithosys_err_t (*p_random_fill)(void*, uint32_t) =
    lithosys_random_fill;
static uint32_t (*p_cpu_count)(void) = lithosys_cpu_count;
static void (*p_exit)(int32_t) = lithosys_exit;
static void (*p_abort)(void) = lithosys_abort;

/* The fixed-width layout the page layer relies on for iov decoding on
 * wasm32 is checked where it can be: field offsets in the struct. */
typedef char lithosys_check_iov_len_is_u32[
    (sizeof(((lithosys_iov_t*)0)->len) == 4) ? 1 : -1];
typedef char lithosys_check_err_is_i32[
    (sizeof(lithosys_err_t) == 4) ? 1 : -1];
typedef char lithosys_check_page_size[
    (LITHOSYS_PAGE_SIZE == 65536u) ? 1 : -1];

/* Referenced so the statics are not diagnosed as unused. */
const void* const lithosys_check_refs[] = {
    (const void*)&p_memory_grow,  (const void*)&p_clock_now_ns,
    (const void*)&p_thread_spawn, (const void*)&p_fd_write,
    (const void*)&p_fd_read,      (const void*)&p_fd_close,
    (const void*)&p_fd_seek,      (const void*)&p_fd_sync,
    (const void*)&p_console_write, (const void*)&p_random_fill,
    (const void*)&p_cpu_count,    (const void*)&p_exit,
    (const void*)&p_abort,
};
