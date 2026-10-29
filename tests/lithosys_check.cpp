// lithosys_check.cpp ... C++ compile check for the LithoSys freeze.
//
// Includes lithosys.h from C++ and takes the address of every frozen
// function through the header's extern "C" surface. If the header's
// C++ guards were missing or wrong, these initializers would fail to
// compile (the names would have C++ linkage and no matching declared
// type). Compile only, for the same reason as the C check.
//
// Build (from the lithoroot directory):
//   c++ -std=c++11 -Wall -Wextra -Werror -I include -c tests/lithosys_check.cpp
#include "lithosys.h"

static lithosys_err_t (*p_memory_grow)(uint32_t, uint32_t*) =
    &lithosys_memory_grow;
static lithosys_err_t (*p_clock_now_ns)(uint32_t, uint64_t*) =
    &lithosys_clock_now_ns;
static int32_t (*p_thread_spawn)(uint32_t) = &lithosys_thread_spawn;
static lithosys_err_t (*p_fd_write)(uint32_t, const lithosys_iov_t*,
                                    uint32_t, uint32_t*) = &lithosys_fd_write;
static lithosys_err_t (*p_fd_read)(uint32_t, const lithosys_iov_t*,
                                   uint32_t, uint32_t*) = &lithosys_fd_read;
static lithosys_err_t (*p_fd_close)(uint32_t) = &lithosys_fd_close;
static lithosys_err_t (*p_fd_seek)(uint32_t, int64_t, uint32_t,
                                   uint64_t*) = &lithosys_fd_seek;
static lithosys_err_t (*p_fd_sync)(uint32_t) = &lithosys_fd_sync;
static void (*p_console_write)(const char*, uint32_t) =
    &lithosys_console_write;
static lithosys_err_t (*p_random_fill)(void*, uint32_t) =
    &lithosys_random_fill;
static uint32_t (*p_cpu_count)(void) = &lithosys_cpu_count;
static void (*p_exit)(int32_t) = &lithosys_exit;
static void (*p_abort)(void) = &lithosys_abort;

extern const void* const lithosys_check_cpp_refs[];
const void* const lithosys_check_cpp_refs[] = {
    (const void*)&p_memory_grow,  (const void*)&p_clock_now_ns,
    (const void*)&p_thread_spawn, (const void*)&p_fd_write,
    (const void*)&p_fd_read,      (const void*)&p_fd_close,
    (const void*)&p_fd_seek,      (const void*)&p_fd_sync,
    (const void*)&p_console_write, (const void*)&p_random_fill,
    (const void*)&p_cpu_count,    (const void*)&p_exit,
    (const void*)&p_abort,
};
