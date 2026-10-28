// lk_web.h — the web platform services, in-house edition.
//
// Every entry point the web build needs from its host page is declared
// here, under our own lk_* names, and implemented by our own page layer
// (wasm/full/lkboot.js). These are plain imports: if the page layer does
// not provide one, the module fails to instantiate — loudly, at load —
// instead of degrading silently at first use. That asymmetry is the
// point of the 2026-10-28 purge: the previous mechanism carried these
// services as address-keyed script snippets whose absence was answered
// with a cached zero-returning stub.
//
// The declarations carry the compiler's import annotation
// (import_module "env"), which is what makes the linker keep them as
// module imports: no link-time JS library declares them, the page
// layer alone provides them. Two entries are NOT imports and carry no
// annotation: lk_gl_attrs_init and lk_gl_proc_address are defined in
// window_web.cpp itself.
//
// Services that must run on the browser main thread (DOM listeners,
// canvas geometry, URL parameters) are dispatched there by the page
// layer; the call sites stay synchronous with the same return values
// the snippets produced. See window_web.cpp for the call sites and
// lkboot.js for the implementations.
//
// The whole header is guarded to the web target: a native translation
// unit that includes it gets an empty header and cannot be affected.
#pragma once

#if defined(__wasm__)

#include <cstddef>

#define LK_WEB_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

#ifdef __cplusplus
extern "C" {
#endif

// ---- input (window_web.cpp's HTML5 backend) ----
// Page URL carries ?debuginput=1 (or the page sets its debug flag).
LK_WEB_IMPORT(lk_input_debug_flag) int lk_input_debug_flag(void);
// Install the DOM listeners + the legacy string queue + the ring
// writer. Once-guarded page-side; must run before lk_input_ring_publish.
LK_WEB_IMPORT(lk_input_install) void lk_input_install(void);
// Publish the shared-memory input ring to the installed listeners.
// Returns 1 when the ring is live, 0 when the page has no shared
// memory (no SharedArrayBuffer / Atomics) — the legacy queue carries.
LK_WEB_IMPORT(lk_input_ring_publish) int lk_input_ring_publish(int base, int capWords, int totalWords);
// Drain the legacy string queue into buf (cap bytes incl. NUL):
// returns the byte count, 0 when empty, -1 when the pending records
// do not fit (the queue is left intact for the next drain).
LK_WEB_IMPORT(lk_input_drain_queue) int lk_input_drain_queue(char* buf, int cap);
// Mirror of the game's cursor-capture want, read by the mousedown
// listener (the reliable pointer-lock requester). Transitions only.
LK_WEB_IMPORT(lk_input_set_capture_wanted) void lk_input_set_capture_wanted(int on);

// ---- window / page ----
// ?aa=on|4 in the page URL: 1, 4, or 0 (no multisampling request).
LK_WEB_IMPORT(lk_page_aa_mode) int lk_page_aa_mode(void);
LK_WEB_IMPORT(lk_window_set_title) void lk_window_set_title(const char* utf8);
LK_WEB_IMPORT(lk_canvas_set_size) void lk_canvas_set_size(int w, int h);
// 0 on success (w/h written), nonzero when the canvas is absent.
LK_WEB_IMPORT(lk_canvas_get_size) int lk_canvas_get_size(int* w, int* h);
LK_WEB_IMPORT(lk_pointer_lock_request) void lk_pointer_lock_request(void);
LK_WEB_IMPORT(lk_pointer_lock_exit) void lk_pointer_lock_exit(void);

// ---- present / host ----
// 1 unless ?lkthin=off in the page URL (software presenter A/B switch).
LK_WEB_IMPORT(lk_present_thin_ok) int lk_present_thin_ok(void);
// 1 when running under Node (bench harness), else 0.
LK_WEB_IMPORT(lk_host_is_node) int lk_host_is_node(void);
// performance.now() in milliseconds (the web step clock's timebase).
LK_WEB_IMPORT(lk_now_ms) double lk_now_ms(void);
// Frame-loop registration: the page asks the callback for one frame
// per display refresh; with simulateInfiniteLoop set, registration
// does not return (the caller's main() unwinds, exactly as the
// present contract requires — see app/main.cpp's FrameCtx note).
LK_WEB_IMPORT(lk_frame_loop_set) void lk_frame_loop_set(void (*cb)(void*), void* arg, int fps, int simulateInfiniteLoop);
LK_WEB_IMPORT(lk_frame_loop_cancel) void lk_frame_loop_cancel(void);

// ---- WebGL context ----
// Attribute block for lk_gl_create_context. The layout is read by byte
// offset in the page layer and must not drift: bools are one byte,
// powerPreference / proxyContextToMainThread are 4-byte enums.
typedef struct LkGlContextAttrs {
  bool alpha;                        // 0
  bool depth;                        // 1
  bool stencil;                      // 2
  bool antialias;                    // 3
  bool premultipliedAlpha;           // 4
  bool preserveDrawingBuffer;        // 5
  int powerPreference;               // 8: 0 default, 1 low-power, 2 high-performance
  bool failIfMajorPerformanceCaveat; // 12
  int majorVersion;                  // 16
  int minorVersion;                  // 20
  bool enableExtensionsByDefault;   // 24
  bool explicitSwapControl;          // 25
  int proxyContextToMainThread;      // 28: 0 disallow, 1 fallback, 2 always
  bool renderViaOffscreenBackBuffer; // 32
  bool desynchronized;               // 33
} LkGlContextAttrs;
// Fill with the shipped defaults (implemented in window_web.cpp).
void lk_gl_attrs_init(LkGlContextAttrs* attrs);
// Create a WebGL context on the page canvas; returns a positive
// handle, or 0 on failure. Make it current with lk_gl_make_current.
LK_WEB_IMPORT(lk_gl_create_context) int lk_gl_create_context(const LkGlContextAttrs* attrs);
LK_WEB_IMPORT(lk_gl_make_current) int lk_gl_make_current(int handle);

#ifdef __cplusplus
}  // extern "C"
#endif

// The GL entry-point address table (implemented in window_web.cpp from
// the loader's own X-macro list; see engine/src/render/gl33/
// lk_gl_imports.h). Returns the address of the imported lk_gl* function
// for a "gl<Name>" query, or null for names WebGL2 cannot provide —
// those resolve to the loader fixups' in-house shims, exactly as the
// loader has always handled them.
extern "C" void* lk_gl_proc_address(const char* name);

static_assert(sizeof(LkGlContextAttrs) == 36, "LkGlContextAttrs layout is an ABI with the page layer");
static_assert(offsetof(LkGlContextAttrs, powerPreference) == 8, "LkGlContextAttrs layout drift");
static_assert(offsetof(LkGlContextAttrs, majorVersion) == 16, "LkGlContextAttrs layout drift");
static_assert(offsetof(LkGlContextAttrs, proxyContextToMainThread) == 28, "LkGlContextAttrs layout drift");
static_assert(offsetof(LkGlContextAttrs, desynchronized) == 33, "LkGlContextAttrs layout drift");

#undef LK_WEB_IMPORT

#endif  // defined(__wasm__)
