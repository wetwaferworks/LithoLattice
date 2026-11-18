# engine/lithoroot/CMakeLists.txt ... the LithoRoot runtime for the LK_WEB link.
#
# Web only (engine/CMakeLists.txt adds this directory under LK_WEB and nowhere else; native
# builds use their own C and C++ libraries). The translation-unit lists and per-TU flags are the
# ones toolchain/build-libs.sh proved; keep the two in step.
#
#   lithoroot_crt1   STATIC  src/crt/crt1.c (start-up: default heap, main thread, errno)
#   lithoroot_c      STATIC  LithoLibC + WaferHeap + LithoWeave + LithoThread + the in-house
#                            compiler-builtin subset (src/rt)
#   lithoroot_cxx    STATIC  LithoAbi + LithoLibC's operator new/delete (weak, replaceable)
#   lithoroot_runtime INTERFACE  all three, plus -nostdlib; linked into every web target
#
# Nothing external: no libc, no libc++, no compiler-rt. wasm-ld (lld) resolves archive members
# regardless of order, so the C and C++ archives may reference each other.
if(NOT LK_WEB)
  message(FATAL_ERROR "engine/lithoroot/CMakeLists.txt is the LK_WEB runtime only")
endif()

set(_lr ${CMAKE_CURRENT_SOURCE_DIR})
set(_lr_c_tus
  src/waferheap.c
  src/libc/bigint.c src/libc/ctype.c src/libc/dtoa.c src/libc/malloc.c src/libc/math.c
  src/libc/memory.c src/libc/printf.c src/libc/scanf.c src/libc/setjmp.c src/libc/stdio.c
  src/libc/stdlib.c src/libc/strerror.c src/libc/string.c src/libc/strings.c src/libc/strtod.c
  src/libc/time.c src/libc/lithovfs.c
  src/lithoweave.c src/lithothread.c src/lithothread_errno.c src/lithothread_heap.c
  src/rt/int128.c)
set(_lr_cxx_tus
  src/libc/new_delete.cc
  src/lithoabi/atexit.cc src/lithoabi/backend_wasm.cc src/lithoabi/eh_runtime.cc
  src/lithoabi/exception.cc src/lithoabi/guard.cc src/lithoabi/personality.cc
  src/lithoabi/std_exceptions.cc src/lithoabi/stubs.cc src/lithoabi/typeinfo.cc
  src/lithovfs_fs.cc)
list(TRANSFORM _lr_c_tus PREPEND ${_lr}/)
list(TRANSFORM _lr_cxx_tus PREPEND ${_lr}/)

# build-libs.sh: C99, -ffreestanding -fno-builtin (LithoLibC implements memcpy and friends, so the
# compiler must not turn their loops back into self-calls), LithoThread owns errno.
set(_lr_c_flags -std=c99 -O1 -ffreestanding -fno-builtin)
set(_lr_cxx_flags -std=c++17 -O1)
# S7 std::filesystem ops: <filesystem> needs the engine's C++20 (chrono, <compare>).
set_source_files_properties(${_lr}/src/lithovfs_fs.cc PROPERTIES COMPILE_OPTIONS -std=c++20)

add_library(lithoroot_crt1 STATIC ${_lr}/src/crt/crt1.c)
target_compile_options(lithoroot_crt1 PRIVATE ${_lr_c_flags} -Wno-prio-ctor-dtor)
target_compile_definitions(lithoroot_crt1 PRIVATE LITHOROOT_WITH_THREAD)

add_library(lithoroot_c STATIC ${_lr_c_tus})
target_compile_options(lithoroot_c PRIVATE ${_lr_c_flags})
target_compile_definitions(lithoroot_c PRIVATE LITHOROOT_WITH_THREAD)
set_source_files_properties(${_lr}/src/libc/setjmp.c PROPERTIES COMPILE_OPTIONS "-fwasm-exceptions")
set_source_files_properties(${_lr}/src/lithoweave.c ${_lr}/src/lithothread.c
  PROPERTIES COMPILE_OPTIONS "-mllvm;-wasm-enable-sjlj")

add_library(lithoroot_cxx STATIC ${_lr_cxx_tus})
target_compile_options(lithoroot_cxx PRIVATE ${_lr_cxx_flags})

add_library(lithoroot_runtime INTERFACE)
# crt1 is an archive member like the rest. Its start-up is a constructor nothing calls by name,
# and wasm-ld drops the constructors of an object with no live symbol, so the module exports the
# start-up status word (0 = heap, main thread and errno ready; 1 = heap only; -1 = never ran).
# That export keeps crt1 live and lets the host check start-up after __wasm_call_ctors.
target_link_libraries(lithoroot_runtime INTERFACE lithoroot_crt1 lithoroot_cxx lithoroot_c)
target_link_options(lithoroot_runtime INTERFACE -nostdlib "LINKER:--export=__lithoroot_crt_status")
