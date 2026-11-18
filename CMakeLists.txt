# Cortana 2026-10-15 0440/0504: PORT symlink fallback, MSVC stack (see CHANGES.md)
cmake_minimum_required(VERSION 3.16)
# LithoLattice engine (src/, assets/) and the LithoKraft game built on it (LithoKraft/).
project(LithoLattice VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 2026-10-25 (JD): DebugOpt = optimised code (/O2 /Ob2, -O2) with debug info and the release CRT,
# but NDEBUG is NOT defined, so every debug log line, assert and diagnostic stays (Vulkan
# validation included). Debug proper (/Od /RTC1 + debug CRT) is ~4 FPS on a full world; this one
# is for playing with Debug's logs. LK_DEBUGOPT=1 marks it. Release / Debug are unchanged.
if(CMAKE_CONFIGURATION_TYPES AND NOT "DebugOpt" IN_LIST CMAKE_CONFIGURATION_TYPES)
  list(APPEND CMAKE_CONFIGURATION_TYPES DebugOpt)
  set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Build configurations" FORCE)
endif()
if(MSVC)
  set(_lk_dbgopt "/O2 /Ob2 /Zi")
else()
  set(_lk_dbgopt "-O2 -g")
endif()
set(CMAKE_C_FLAGS_DEBUGOPT "${_lk_dbgopt}" CACHE STRING "DebugOpt C flags (no NDEBUG)" FORCE)
set(CMAKE_CXX_FLAGS_DEBUGOPT "${_lk_dbgopt}" CACHE STRING "DebugOpt C++ flags (no NDEBUG)" FORCE)
foreach(_k EXE SHARED MODULE STATIC)
  set(CMAKE_${_k}_LINKER_FLAGS_DEBUGOPT "${CMAKE_${_k}_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "" FORCE)
endforeach()
add_compile_definitions($<$<CONFIG:DebugOpt>:LK_DEBUGOPT=1>)

# Static MSVC runtime for everything (GLFW too) so LithoKraft.exe needs no extra DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get extraction-time timestamps
endif()
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
  # X11 only keeps the Linux dependency list short; Wayland can be enabled later.
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()
# LK_WEB: the browser build, i.e. the LithoRoot wasm32 toolchain (cmake/lk-wasm32-toolchain.cmake,
# stock clang). Keyed off the target processor, never the EMSCRIPTEN variable, as render/CMakeLists.txt
# already does (JD's strict rule, 2026-10-28: no Emscripten, ever). Native builds: OFF, nothing changes.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(wasm32|wasm64)$")
  set(LK_WEB ON)
else()
  set(LK_WEB OFF)
endif()
# WASM checkpoint (2026-10-18): no GLFW in the browser build — the platform
# module swaps in window_web.cpp (see src/platform/CMakeLists.txt).
if(NOT LK_WEB)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 7b6aead9fb88b3623e3b3725ebb42670cbe4c579  # Cortana 2026-10-15 1500 (TOOLS, MC-06): GLFW 3.4, pinned by commit
  )
FetchContent_MakeAvailable(glfw)
endif()

# LK_WEB (link wiring, 2026-11-16): LithoRoot is the whole runtime of the web module. Its crt1,
# LithoLibC and C++ runtime are built here and linked into every target defined after this
# point; the toolchain's -nostdlib keeps clang from adding any libc, libc++ or compiler-rt of
# its own. Native builds never enter lithoroot/.
if(LK_WEB)
  add_subdirectory(lithoroot)
  link_libraries(lithoroot_runtime)
endif()

# AngelScript 2.38.0, official SDK zip from angelcode.com, pinned by SHA256.
# LK_WEB: AngelScript is the ONE sanctioned third-party library in the web build (JD 2026-11-16
# 12:41 ET, "grant an exception"); everything else third-party stays off LK_WEB.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)   # honour the static MSVC runtime in the SDK's older CMake project
set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(angelscript
  URL https://www.angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip
  URL_HASH SHA256=b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f
  SOURCE_SUBDIR angelscript/projects/cmake)  # the zip's top folder sdk/ is stripped
# LK_WEB (2026-11-16): the SDK's CMake project links Threads::Threads. On LithoRoot that would end
# up as -lpthreads at the final link (FindThreads only try-compiles static libraries here), and the
# web AngelScript runs without threads anyway, so the target is an empty stand-in.
if(LK_WEB AND NOT TARGET Threads::Threads)
  add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
endif()
FetchContent_MakeAvailable(angelscript)
# LK_WEB: generic calling convention only, no AngelScript threads (as_config.h picks both for an
# unknown platform; spelled out so every TU that includes angelscript.h agrees). PUBLIC on purpose.
if(LK_WEB)
  target_compile_definitions(angelscript PUBLIC AS_MAX_PORTABILITY AS_NO_THREADS=)
endif()

# stb_image v2.30 (single header), official github.com/nothings/stb, pinned commit + SHA256.
# Not on LK_WEB (JD 12:41 ET: in-house PNG/SVG decoder; until then image.cpp fails safe there).
set(LITHOLATTICE_STB_DIR ${CMAKE_BINARY_DIR}/_deps/stb)
if(NOT LK_WEB AND NOT EXISTS ${LITHOLATTICE_STB_DIR}/stb_image.h)
  file(DOWNLOAD
    https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
    ${LITHOLATTICE_STB_DIR}/stb_image.h
    EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    TLS_VERIFY ON)
endif()

# miniaudio 0.11.25 (single header), official github.com/mackron/miniaudio release tag + SHA256.
# Not on LK_WEB (JD 12:41 ET: our own AudioWorklet output; until then the web build runs muted).
set(LITHOLATTICE_MINIAUDIO_DIR ${CMAKE_BINARY_DIR}/_deps/miniaudio)
if(NOT LK_WEB AND NOT EXISTS ${LITHOLATTICE_MINIAUDIO_DIR}/miniaudio.h)
  file(DOWNLOAD
    https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
    ${LITHOLATTICE_MINIAUDIO_DIR}/miniaudio.h
    EXPECTED_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    TLS_VERIFY ON)
endif()

# Vulkan backend dependencies (official KhronosGroup release archives, pinned by SHA256).
# Headers only: the backend loads the system Vulkan loader (vulkan-1.dll / libvulkan.so.1, shipped
# with the GPU driver) at run time, so nothing Vulkan is linked and the game still starts without it.
if(NOT LK_WEB)  # no Vulkan backend in the browser build (render_vk is off LK_WEB too)
FetchContent_Declare(vulkan_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e)
FetchContent_MakeAvailable(vulkan_headers)
endif()

# glslang 16.6.0: the reference GLSL -> SPIR-V compiler, built as a host tool so the Vulkan
# shaders (assets/shaders/vk) compile at build time. No optimizer / HLSL / tests / install.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# WASM checkpoint (2026-10-18): glslang is a host tool for the Vulkan backend's
# SPIR-V step; neither is built for the browser (render_vk is skipped below).
if(NOT LK_WEB)
FetchContent_Declare(glslang
  URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
  URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2)
# Third-party build tool: its own GCC/Clang warnings are not ours to fix, so they are silenced for
# glslang's directories only (flags restored right after). MSVC builds it warning-free as is
# (adding /w there would itself warn, D9025).
set(LITHOLATTICE_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
if(NOT MSVC)
  string(APPEND CMAKE_CXX_FLAGS " -w")
endif()
FetchContent_MakeAvailable(glslang)
set(CMAKE_CXX_FLAGS "${LITHOLATTICE_SAVED_CXX_FLAGS}")
endif()

# One static library per engine module; dependencies are one-way and never reach the game:
#   {sim, world, render, platform, script, audio, input, ui} -> core   (render -> world read-only; sim -> world;
#   platform -> input: the GLFW input backend; world -> procgen -> core: noise, graphs, generator pipeline)
#   render -> ui -> core (render implements ui::IUiRenderer); render -> assets -> core
#   audio_device (miniaudio backend) -> audio; render_vk (Vulkan backend) -> render
# The game (LithoKraft/): lithokraft (exe) -> lithokraft_game -> engine modules.
# Optimisation comes from CMake's per-config flags (Release: /O2 or -O3 and NDEBUG; Debug: /Od /RTC1
# or -O0 -g), never forced here: /O2 next to Debug's /RTC1 is MSVC error D8016. GCC/Clang DebugOpt
# and RelWithDebInfo keep -O2 as before; Release uses CMake's -O3 (a trailing -O2 was clobbering it). LITHOLATTICE_WERROR (off by default, on in CI) turns warnings into errors.
option(LITHOLATTICE_WERROR "Treat warnings in LithoLattice / LithoKraft code as errors" OFF)
# Verbose thread/job-system debug logging (job lifecycle, steals, lock order, wake/sleep,
# queue depths, affinity). Compile-time gated: zero overhead when off. The self-tests
# build and pass both ways.
option(LITHOLATTICE_THREAD_DEBUG "Verbose compile-time thread/job-system debug logging" OFF)
# GCC / Clang: LITHOLATTICE_SANITIZE=address (ASan + UBSan) or thread (TSan) for test runs.
set(LITHOLATTICE_SANITIZE "" CACHE STRING "Sanitizer for GCC/Clang builds: address | thread | empty")
if(LITHOLATTICE_SANITIZE AND NOT MSVC)
  if(LITHOLATTICE_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  elseif(LITHOLATTICE_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "LITHOLATTICE_SANITIZE must be address or thread")
  endif()
endif()
# Windows: the default stack is 1 MB (main thread and std::thread) vs 8 MB on Linux;
# deep recursion in the audio tests and --selftest overflowed it. Match Linux's 8 MB.
if(MSVC)
  add_link_options(/STACK:8388608)
endif()

# Stricter warnings (review 2026-10-24 W-4): on top of /W4 / -Wall -Wextra, the
# undefined-behaviour class (shift count out of range, missing return, uninitialised
# local) is now an error (zero hits on MSVC today); a polymorphic class with a non-virtual
# destructor warns (C4265 / -Wnon-virtual-dtor), and so does a shadowed name on GCC/Clang
# (-Wshadow; MSVC /W4 already reports C4456-C4459). Native builds only: the Emscripten
# web build keeps its flags.
function(litholattice_module_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /w14265 /we4293 /we4715 /we4716 /we4700
                           $<$<BOOL:${LITHOLATTICE_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra $<$<OR:$<CONFIG:DebugOpt>,$<CONFIG:RelWithDebInfo>>:-O2>
                           $<$<BOOL:${LITHOLATTICE_WERROR}>:-Werror>)
    if(NOT LK_WEB)
      target_compile_options(${target} PRIVATE -Wshadow -Wnon-virtual-dtor
                             -Werror=shift-count-overflow -Werror=return-type)
    endif()
  endif()
endfunction()

# Ginny 2026-10-16 (perf/determinism list, Cortana): pin floating-point semantics on the
# targets reachable from the sim tick. MSVC: /fp:precise stated explicitly (it is the default;
# never /fp:fast) - MSVC x64 does not contract to FMA without /fp:contract. GCC/Clang:
# -ffp-contract=off (GCC defaults to fast, Clang to on), so ARM64/FMA hosts round like x86.
function(litholattice_fp_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /fp:precise)
  else()
    target_compile_options(${target} PRIVATE -ffp-contract=off)
  endif()
endfunction()

add_subdirectory(src/core)
add_subdirectory(src/procgen)
add_subdirectory(src/ui)
add_subdirectory(src/assets)
add_subdirectory(src/audio)
add_subdirectory(src/input)
add_subdirectory(src/platform)
add_subdirectory(src/script)
add_subdirectory(src/mods)
add_subdirectory(src/console)
add_subdirectory(src/crypto)  # Ed25519/SHA-2 for mod signing
add_subdirectory(src/index)
add_subdirectory(src/mcp)
add_subdirectory(src/defrag)
add_subdirectory(src/player)
add_subdirectory(src/net)
add_subdirectory(src/projectile)
add_subdirectory(src/world)
add_subdirectory(src/sim)
add_subdirectory(src/entity)
add_subdirectory(src/ai)
add_subdirectory(src/anim)
add_subdirectory(src/storage)
add_subdirectory(src/subdiv)  # sparse-octree sub-block subdivision (render/detail-level only)
add_subdirectory(src/texture)  # clean-room procedural texture generation (recipes, tileable noise/voronoi)
add_subdirectory(src/render)
# WASM checkpoint (2026-10-18): no Vulkan backend in the browser build.
if(NOT LK_WEB)
add_subdirectory(src/render_vk)
endif()
# The game (LithoKraft/) builds on the engine modules above; nothing in src/ depends on it.
enable_testing()  # ctest: engine tests (tests/, one per module) and the game selftest (LithoKraft/src/app)
add_subdirectory(src/camera)  # engine-level virtual camera system; after enable_testing() so its add_test registers
add_subdirectory(src/systerian)  # certificate-signing script codec; after enable_testing() so its add_test registers
add_subdirectory(src/sprite)  # sprite registry/atlas + instances + particle emitters; after enable_testing() so its add_test registers
option(LITHOLATTICE_BUILD_TESTS "Build the engine-only test executable litholattice_tests" ON)
if(LITHOLATTICE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
option(LITHOLATTICE_BUILD_FUZZERS "Build the litholattice_fuzz harnesses (tools/fuzz)" OFF)
if(LITHOLATTICE_BUILD_FUZZERS)
  add_subdirectory(tools/fuzz)
endif()
# Game hookup (2026-10-29, engine repo split). LithoLattice is its own repo; each game repo
# (LithoKraft, LithoStrike, LithoKart) mounts it as a git submodule at <game repo>/engine and
# keeps its game at <game repo>/game/<Name>. Configure with `cmake -S engine -B build` from the
# game repo, exactly as before. LL_GAME_NAME picks the game; empty auto-detects the one present.
# With no game next to it (the engine repo on its own) only the engine modules and tests build.
set(LL_GAME_NAME "" CACHE STRING "Game under ../game to build on the engine (LithoKraft, LithoStrike, LithoKart); empty = auto-detect")
set(LL_GAME_DIR "")
if(LL_GAME_NAME)
  get_filename_component(LL_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../game/${LL_GAME_NAME}" ABSOLUTE)
  if(NOT EXISTS "${LL_GAME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "LL_GAME_NAME=${LL_GAME_NAME}: ${LL_GAME_DIR}/CMakeLists.txt not found")
  endif()
else()
  foreach(_ll_g LithoKraft LithoStrike LithoKart)
    # engine/<Name> is a symlink on Linux/macOS; Windows unzips it as a plain file, so fall back to ../game/<Name>.
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_ll_g}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_ll_g}/CMakeLists.txt")
      set(LL_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${_ll_g}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../game/${_ll_g}/CMakeLists.txt")
      get_filename_component(LL_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../game/${_ll_g}" ABSOLUTE)
    endif()
    if(LL_GAME_DIR)
      set(LL_GAME_NAME "${_ll_g}")
      break()
    endif()
  endforeach()
endif()
set(LL_GAME_TARGET "")
if(LL_GAME_DIR)
  string(TOLOWER "${LL_GAME_NAME}" LL_GAME_TARGET)  # lithokraft / lithostrike / lithokart: the game exe target
  set(LITHOKRAFT_GAME_DIR "${LL_GAME_DIR}")  # old name, kept for scripts that read it
  message(STATUS "LithoLattice: building game ${LL_GAME_NAME} from ${LL_GAME_DIR}")
  add_subdirectory("${LL_GAME_DIR}" game)  # binary dir "game": the game exe sits in the build root
else()
  message(STATUS "LithoLattice: no game found next to the engine; building the engine and its tests only")
endif()

# 2026-10-18 (JD): the rolling build counter. Eight wheels of 0..63 (64^8 = 2^48 builds);
# EVERY build invocation rolls the rightmost wheel +1, carrying left at 64 (JD's law, the
# same odometer as his redstone subscriber counter). State lives in the tracked
# <tree>/BUILD_COUNTER.txt so it survives build-dir wipes; every roll is appended to the
# <tree>/BUILD_COUNTER.log ledger. Display/log only: it never feeds the determinism hash.
# The custom target is in ALL and a dependency of the game exe, so it runs on every build
# (even when nothing recompiles) and before the exe's sources compile.
find_program(LITHOLATTICE_PYTHON3 NAMES python3 python)
# 2026-10-23 (JD, fix run 2): builds NEVER write the engine counter by default; it is
# set by hand (see docs/versioning/COUNTERS.md). The target only regenerates the header.
option(LL_ENGINE_COUNTER_BUMP "Roll BUILD_COUNTER.txt on every build (off: read-only)" OFF)
set(_jd_counter_mode --no-bump)
if(LL_ENGINE_COUNTER_BUMP)
  set(_jd_counter_mode)
endif()
get_filename_component(JD_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)  # the game repo root (engine/ is the submodule)
if(LITHOLATTICE_PYTHON3 AND EXISTS "${JD_TREE_ROOT}/tools/build_counter.py")
  add_custom_target(jd_build_counter ALL
    COMMAND "${LITHOLATTICE_PYTHON3}" "${JD_TREE_ROOT}/tools/build_counter.py" ${_jd_counter_mode}
            --state "${JD_TREE_ROOT}/BUILD_COUNTER.txt"
            --header "${CMAKE_BINARY_DIR}/generated/build_counter.h"
            --log "${JD_TREE_ROOT}/BUILD_COUNTER.log"
    COMMENT "JD rolling build counter"
    VERBATIM)
  if(LL_GAME_TARGET AND TARGET ${LL_GAME_TARGET})
    add_dependencies(${LL_GAME_TARGET} jd_build_counter)
    target_include_directories(${LL_GAME_TARGET} PRIVATE "${CMAKE_BINARY_DIR}/generated")
  endif()
else()
  message(STATUS "build counter disabled (python3 or <tree>/tools/build_counter.py not found)")
endif()

# LithoCEF host shell (JD, 2026-10-20): "The CEF layer will sit OUTSIDE of the
# engine and just boot the engine. And the CEF layer plugs into the hardware
# layer." The shell lives in <tree>/host/lithocef — outside this project —
# and is added only on request. Default OFF: the default build gains no
# targets, no code, and no behavior. The stub backend proves the ILithoCEF
# contract standalone; it never links into the engine or the game.
option(LL_WITH_LITHOCEF "Build the LithoCEF host-shell stub backend (host/lithocef)" OFF)
if(LL_WITH_LITHOCEF AND NOT LK_WEB)
  get_filename_component(LITHOCEF_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
  add_subdirectory("${LITHOCEF_TREE_ROOT}/host/lithocef" "${CMAKE_BINARY_DIR}/host/lithocef")
endif()
