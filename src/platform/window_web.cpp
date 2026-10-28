// Web (browser) implementation of the Window interface — WASM checkpoint,
// 2026-10-18 overnight; input backend landed 2026-10-19 (JD playtest report #1:
// "controls don't work in the web browser"). NEW FILE: the native window.cpp is
// untouched and still builds everywhere else; CMake selects this file instead
// of window.cpp + glfw_input_backend.cpp only for the web build (see
// src/platform/CMakeLists.txt).
//
// Scope, stated honestly:
// - The page's <canvas id="canvas"> is resized to the requested window size and
//   framebufferSize() reports the canvas element size.
// - Input is a REAL backend (class WebHtml5InputBackend below): DOM
//   keyboard (document-level), mouse + wheel (canvas-level), pointer lock for
//   cursor capture, window focus edges — translated into the same InputEvent
//   stream the GLFW backend produces natively, with the same semantics
//   (motion deltas always fed; the game applies them when it has the cursor
//   captured; FocusLost releases everything held). Transport: main-thread
//   JS listeners + a per-frame drain (see the transport note at the backend);
//   host callback registration delivery proved dead in this configuration.
// - swap() is a no-op: in the browser the canvas is composited by the page
//   — but only at task boundaries. Build 50 note: presentation is NOT
//   this function's job; the app's frame loop must yield (on web it is
//   driven by lk_frame_loop_set — see app/main.cpp FrameCtx). An explicit
//   commit-frame call is a documented no-op in current browsers ('swap
//   is implicit'), and explicitSwapControl changes nothing without it;
//   both were tried and reverted 2026-10-19.
// - glProcAddress() answers from a table of the page layer's lk_gl*
//   imports (lk_gl_imports.h), so the GL33 loader pattern compiles and
//   links unchanged. For OpenGL33 windows create() also creates a real
//   WebGL2 (GLES3) context on the canvas and makes it current, mirroring
//   the native contract ("the GL 3.3 core context is current on
//   return") — the software presenter's WebGL variant and the GL33
//   backend both build on it. Whether the GL33 renderer's GLSL 330
//   shaders run is the pack's business (ES 3.00 translations ride the
//   wasm pack overlay), not this file's.
// - No Vulkan in the browser: surface/extensions report unavailable, and the
//   app's Vulkan request falls back exactly as it does on a Vulkan-less PC.
//
// All host services are our own named imports, declared in
// litholattice/platform/lk_web.h and implemented in the page layer
// (wasm/full/lkboot.js) — the 2026-10-28 purge replaced the previous
// address-keyed script-snippet mechanism wholesale.
#include "litholattice/platform/window.h"

#include "litholattice/platform/lk_web.h"

#include "lk_gl_imports.h"  // render/gl33: the lk_gl* import declarations + loader X-macro

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include "litholattice/core/log.h"

namespace litholattice {

namespace {

double nowSeconds() { return lk_now_ms() / 1000.0; }

/// State shared between the input backend's poll() (which converts the
/// main-thread listener records into events) and the Window (cursor
/// capture + focus). Everything is touched on this (proxied) pthread; the
/// mutex is belt-and-braces, not load-bearing.
struct InputSharedState {
  std::mutex mutex;
  std::vector<InputEvent> queue;
  std::atomic<bool> focused{true};
  std::atomic<bool> pointerLocked{false};
  std::atomic<bool> captureWanted{false};
  std::atomic<int> focusLostEdges{0};
  std::atomic<int> focusGainedEdges{0};
  std::atomic<double> lastLockRequest{-1.0};
  std::atomic<double> lastLockRelease{-1.0};  // drain-fix: post-exit pointer-lock cooldown guard
  std::atomic<bool> lockRefusalLogged{false};  // build 80: refusal logs once per capture session, not per tap
  bool debugEcho = false;       // ?debuginput=1 in the page URL
  int moveEchoBudget = 0;       // rate-limited mousemove echo (debug only)
  bool haveCursor = false;      // last canvas-relative cursor position valid
  double lastX = 0, lastY = 0;  // CSS pixels, canvas-relative
  double cssW = 0, cssH = 0;    // canvas CSS size, refreshed by every drain
  int fbW = 0, fbH = 0;         // canvas framebuffer size, same refresh

  void push(const InputEvent& e) {
    std::lock_guard lock(mutex);
    queue.push_back(e);
  }
};

/// DOM KeyboardEvent.code (physical key position, layout independent — the
/// same basis GLFW key codes use) -> litholattice Key.
Key keyFromDomCode(const char* code) {
  const std::string_view c = code;
  if (c.size() == 4 && c.starts_with("Key")) {
    const char l = c[3];
    if (l >= 'A' && l <= 'Z') return Key(int(Key::A) + (l - 'A'));
  }
  if (c.size() == 6 && c.starts_with("Digit")) {
    const char d = c[5];
    if (d >= '0' && d <= '9') return Key(int(Key::Num0) + (d - '0'));
  }
  if (c.size() == 7 && c.starts_with("Numpad")) {
    const char d = c[6];
    if (d >= '0' && d <= '9') return Key(int(Key::Num0) + (d - '0'));
  }
  if (c == "Space") return Key::Space;
  if (c == "Enter" || c == "NumpadEnter") return Key::Enter;
  if (c == "Escape") return Key::Escape;
  if (c == "Backspace") return Key::Backspace;
  if (c == "Tab") return Key::Tab;
  if (c == "ShiftLeft") return Key::LeftShift;
  if (c == "ShiftRight") return Key::RightShift;
  if (c == "ControlLeft") return Key::LeftCtrl;
  if (c == "ControlRight") return Key::RightCtrl;
  if (c == "AltLeft") return Key::LeftAlt;
  if (c == "AltRight") return Key::RightAlt;
  if (c == "ArrowUp") return Key::Up;
  if (c == "ArrowDown") return Key::Down;
  if (c == "ArrowLeft") return Key::Left;
  if (c == "ArrowRight") return Key::Right;
  if (c == "Insert") return Key::Insert;
  if (c == "Delete") return Key::Delete;
  if (c == "Home") return Key::Home;
  if (c == "End") return Key::End;
  if (c == "PageUp") return Key::PageUp;
  if (c == "PageDown") return Key::PageDown;
  if (c == "Minus") return Key::Minus;
  if (c == "Equal") return Key::Equal;
  if (c == "BracketLeft") return Key::LeftBracket;
  if (c == "BracketRight") return Key::RightBracket;
  if (c == "Semicolon") return Key::Semicolon;
  if (c == "Quote") return Key::Apostrophe;
  if (c == "Comma") return Key::Comma;
  if (c == "Period") return Key::Period;
  if (c == "Slash") return Key::Slash;
  if (c == "Backslash" || c == "IntlBackslash") return Key::Backslash;
  if (c == "Backquote") return Key::GraveAccent;
  if (c.size() >= 2 && c[0] == 'F') {  // F1..F12
    int n = 0;
    bool digits = true;
    for (size_t i = 1; i < c.size(); ++i) {
      if (c[i] < '0' || c[i] > '9') { digits = false; break; }
      n = n * 10 + (c[i] - '0');
    }
    if (digits && n >= 1 && n <= 12) return Key(int(Key::F1) + (n - 1));
  }
  return Key::Unknown;
}

std::optional<MouseButton> buttonFromDom(unsigned short b) {
  switch (b) {
    case 0: return MouseButton::Left;
    case 1: return MouseButton::Middle;
    case 2: return MouseButton::Right;
    case 3: return MouseButton::Back;
    case 4: return MouseButton::Forward;
    default: return std::nullopt;
  }
}

/// First UTF-8 code point of a DOM `key` string, or 0 when it is a named
/// (multi-character) key rather than text.
char32_t firstCodePoint(const char* s) {
  const auto* u = reinterpret_cast<const unsigned char*>(s);
  if (!u[0]) return 0;
  if (u[0] < 0x80) return u[0];
  if ((u[0] & 0xE0) == 0xC0 && u[1]) return char32_t(((u[0] & 0x1F) << 6) | (u[1] & 0x3F));
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) return char32_t(((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F));
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3])
    return char32_t(((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F));
  return 0;
}

// ---------------------------------------------------------------------------
// Input transport (rewritten 2026-10-19, fix run for JD's playtest report #1)
//
// The first transport registered C++ callbacks with the host's
// callback-registration API from this (proxied) pthread. In this build
// configuration those callbacks are NEVER delivered: an in-game diagnostic
// heartbeat (poll() counting callback pushes) ran for minutes across two
// browsers runs and recorded pushed=0 while the same page's plain JS
// listeners saw every event, and a standalone probe with identical flags
// DID receive its callbacks — so the mechanism is configuration-sensitive
// in a way we could not pin down and must not depend on. This transport
// uses only pipes proven live in this binary: named-import hops to the
// browser main thread (lk_input_install / lk_input_drain_queue in the
// page layer, the same hop the debug-flag read and Window::setTitle
// already rely on) and poll().
//
// Main-thread JS listeners (installed once by the backend constructor)
// serialize DOM events into window.__lkQ as pipe-delimited lines; poll()
// drains the queue once per frame via one lk_input_drain_queue call that
// also reports the canvas geometry, then converts records to InputEvents with
// exactly the semantics the callback version had (same key map, same
// consume rules, same echoes, same drag-look fallback).
//
// Build 59 adds the memory-mapped ring as the primary transport (see the
// ring block below): when the page is cross-origin isolated, the same
// listeners write binary records into shared WASM memory instead, and
// poll() drains those — the string queue remains as the automatic
// fallback (non-isolated pages, and the boot window before the first
// ring record lands). One conversion core serves both.
//
// Drain fix (build 63, JD's ruling 2026-10-20: keep all three fusion
// legs, fix the drain first): the ring is no longer drained at frame
// boundaries. A dedicated drain thread consumes it at STEP cadence
// (256 wakes/s — the web step clock), so a record's transport latency
// is bounded by one step period (~3.9 ms) instead of one presented
// frame (unbounded under streaming load). Records carry writer-stamped
// event timestamps, and conversion attributes each event to its own
// step. If the drain thread cannot start, poll() falls back to the
// build-62 per-frame drain, unchanged. The legacy string queue's
// behavior is untouched in every mode.
// ---------------------------------------------------------------------------

/// Percent-decode a JS encodeURIComponent string back to raw UTF-8 bytes.
std::string urlDecode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
      out.push_back(char(hexv(s[i + 1]) * 16 + hexv(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::vector<std::string_view> splitFields(std::string_view line) {
  std::vector<std::string_view> f;
  size_t b = 0;
  while (true) {
    const size_t p = line.find('|', b);
    if (p == std::string_view::npos) { f.push_back(line.substr(b)); break; }
    f.push_back(line.substr(b, p - b));
    b = p + 1;
  }
  return f;
}

double fieldNum(const std::vector<std::string_view>& f, size_t i) {
  if (i >= f.size() || f[i].empty()) return 0;
  return std::strtod(std::string(f[i]).c_str(), nullptr);
}
bool fieldFlag(const std::vector<std::string_view>& f, size_t i) { return fieldNum(f, i) != 0; }

/// Shared conversion core for key records (build 59): both transports —
/// the legacy string queue and the memory-mapped ring — decode their wire
/// format into these primitives and call here, so event semantics exist
/// exactly once. `code`/`key` are the raw DOM strings (already decoded).
/// `t` is the event's own timestamp: the legacy path passes conversion
/// time (its historical behavior, unchanged); the ring path passes the
/// writer-stamped event time (drain fix, build 63), so an event is
/// attributed to the step it happened in, not the frame that drained it.
void handleKeyVals(InputSharedState* st, bool down, const char* codeZ, const char* keyZ,
                   bool repeat, bool ctrl, bool meta, double t) {
  const std::string code = codeZ ? codeZ : "";
  const Key k = keyFromDomCode(code.c_str());
  if (down) {
    if (k != Key::Unknown) {
      st->push(InputEvent::key(repeat ? InputEvent::Type::KeyRepeat : InputEvent::Type::KeyDown, k, t));
      if (st->debugEcho) {
        const std::string name(inputName(k));
        logInfo("input> KeyDown %s (code %s%s)", name.c_str(), code.c_str(), repeat ? ", repeat" : "");
      }
    } else if (st->debugEcho) {
      logInfo("input> KeyDown unmapped (code %s)", code.c_str());
    }
    // Text: the DOM `key` string is the post-layout character (GLFW's char
    // callback equivalent). Single printable characters only.
    if (!ctrl && !meta && keyZ) {
      const std::string kv = keyZ;
      if (kv.size() >= 1 && kv.size() <= 4) {
        const char32_t cp = firstCodePoint(kv.c_str());
        if (cp >= 0x20 && cp != 0x7F) st->push(InputEvent::text(cp, t));
      }
    }
  } else {
    if (k != Key::Unknown) {
      st->push(InputEvent::key(InputEvent::Type::KeyUp, k, t));
      if (st->debugEcho) {
        const std::string name(inputName(k));
        logInfo("input> KeyUp %s", name.c_str());
      }
    }
  }
}

void handleKey(InputSharedState* st, bool down, const std::vector<std::string_view>& f) {
  const std::string code = f.size() > 1 ? urlDecode(f[1]) : std::string();
  const std::string kv = f.size() > 2 ? urlDecode(f[2]) : std::string();
  handleKeyVals(st, down, code.c_str(), kv.c_str(), fieldFlag(f, 3), fieldFlag(f, 4), fieldFlag(f, 5),
                nowSeconds());
}

/// Canvas-relative cursor position (CSS px) -> framebuffer pixels, using the
/// geometry the last drain reported (refreshed every frame before use).
void cursorFb(InputSharedState* st, double targetX, double targetY, float& fx, float& fy) {
  fx = float(st->cssW > 0 ? targetX * st->fbW / st->cssW : targetX);
  fy = float(st->cssH > 0 ? targetY * st->fbH / st->cssH : targetY);
}

void handleMouseButtonVals(InputSharedState* st, bool down, int buttonNum, double x, double y, double t) {
  const auto b = buttonFromDom((unsigned short)buttonNum);
  if (b) {
    st->push(InputEvent::mouse(down ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp, *b, t));
    if (st->debugEcho) {
      const std::string name(inputName(*b));
      logInfo("input> Mouse%s %s", down ? "Down" : "Up", name.c_str());
    }
  }
  if (down) {
    float fx = 0, fy = 0;
    cursorFb(st, x, y, fx, fy);
    st->push(InputEvent::cursor(fx, fy, t));
  }
}

void handleMouseButton(InputSharedState* st, bool down, const std::vector<std::string_view>& f) {
  handleMouseButtonVals(st, down, (int)fieldNum(f, 1), fieldNum(f, 2), fieldNum(f, 3), nowSeconds());
}

void handleMouseMoveVals(InputSharedState* st, double targetX, double targetY, double movX, double movY,
                         double t) {
  float dx = 0, dy = 0;
  if (st->pointerLocked.load() && (movX != 0 || movY != 0)) {
    // Locked: the DOM movement is the unbounded delta (position is frozen).
    dx = float(movX);
    dy = float(movY);
    st->haveCursor = false;  // position meaningless while locked
  } else {
    // Unlocked fallback (also the drag-look path when a browser refuses
    // the lock): deltas from successive canvas-relative positions, exactly
    // the GLFW backend's method. Look still turns; it just stops at edges.
    if (st->haveCursor) {
      dx = float(targetX - st->lastX);
      dy = float(targetY - st->lastY);
    }
    st->lastX = targetX;
    st->lastY = targetY;
    st->haveCursor = true;
  }
  if (dx != 0.f || dy != 0.f) {
    st->push(InputEvent::motion(dx, dy, t));
    if (st->debugEcho && st->moveEchoBudget > 0) {
      --st->moveEchoBudget;
      logInfo("input> MouseMove %+.0f %+.0f%s", double(dx), double(dy), st->pointerLocked.load() ? " (locked)" : "");
    }
  }
  if (!st->pointerLocked.load()) {
    float fx = 0, fy = 0;
    cursorFb(st, targetX, targetY, fx, fy);
    st->push(InputEvent::cursor(fx, fy, t));
  }
}

void handleMouseMove(InputSharedState* st, const std::vector<std::string_view>& f) {
  handleMouseMoveVals(st, fieldNum(f, 1), fieldNum(f, 2), fieldNum(f, 3), fieldNum(f, 4), nowSeconds());
}

void handleWheelVals(InputSharedState* st, double deltaY, int deltaMode, double t) {
  // DOM: deltaY down is positive; the game's wheel is + = up / away.
  float steps = 0;
  if (deltaMode == 0) steps = float(-deltaY / 100.0);   // pixels: ~100 px per notch
  else if (deltaMode == 1) steps = float(-deltaY);      // lines
  else steps = float(-deltaY * 5.0);                    // pages
  steps = std::clamp(steps, -8.f, 8.f);
  if (steps != 0.f) {
    st->push(InputEvent::wheel(steps, t));
    if (st->debugEcho) logInfo("input> MouseWheel %+.2f", double(steps));
  }
}

void handleWheel(InputSharedState* st, const std::vector<std::string_view>& f) {
  handleWheelVals(st, fieldNum(f, 1), (int)fieldNum(f, 2), nowSeconds());
}

void handlePointerLockChange(InputSharedState* st, bool active, double t) {
  const bool was = st->pointerLocked.exchange(active);
  if (active) st->lockRefusalLogged = false;  // build 80: a real lock re-arms the one-line refusal report
  logInfo("input: pointer lock %s", active ? "engaged" : "released");
  if (was && !active) {
    // The browser freed the cursor (Esc, or an OS-level grab): mirror the
    // native focus-loss path so the game releases everything it held.
    st->push(InputEvent::focusLost(t));
    st->haveCursor = false;
    st->lastLockRelease = t;  // drain-fix rider: Chrome's post-exit re-lock cooldown runs from here
  }
}

void handlePointerLockError(InputSharedState* st) {
  // Refused (no gesture, or the post-Esc cooldown Chrome enforces): not
  // fatal — position-delta look keeps working, and the next canvas click
  // retries the request from inside its handler.
  // Build 80: the refusal is logged ONCE per capture session, not per
  // event. On touch-primary devices the browser refuses every tap's
  // compatibility-mouse request by design (and lkboot + the listener now
  // skip the request there entirely); one line names the fallback and
  // the latch re-arms when a lock actually engages.
  if (!st->lockRefusalLogged.exchange(true))
    logInfo("input: pointer lock refused by the browser (fallback: position-delta look; click the game to retry)");
  st->lastLockRequest = nowSeconds();  // engage the frame-loop throttle too
  st->lastLockRelease = nowSeconds();  // and the cooldown guard (drain-fix rider)
}

void handleFocus(InputSharedState* st, bool gained, double t) {
  if (gained) {
    st->focused = true;
    ++st->focusGainedEdges;
    st->haveCursor = false;  // no motion jump from where the cursor went while away
    st->push(InputEvent::focusGained(t));
    if (st->debugEcho) logInfo("input> FocusGained");
  } else {
    st->focused = false;
    ++st->focusLostEdges;
    st->push(InputEvent::focusLost(t));
    if (st->debugEcho) logInfo("input> FocusLost");
  }
}

// ---------------------------------------------------------------------------
// Memory-mapped input ring (build 59; design note Web-Seam-Design-2026-10-20,
// proposed and approved by JD 2026-10-20 00:27 EDT)
//
// A browser WASM module has no syscalls; its host imports are the syscall
// table, and the closest thing to memory-mapped I/O the platform offers is
// a fixed region of shared linear memory written by the host. This ring is
// that region. The main-thread DOM listeners write fixed-size binary
// records into it with Atomics (single producer); a single consumer
// drains them and converts through the same handlers the legacy queue
// feeds, so InputEvent semantics are unchanged. No per-frame bridge hop
// and no string serialization on this path.
//
// Drain cadence (build 63): the consumer is a dedicated drain thread
// waking at STEP boundaries (256/s, the web step clock) — input is no
// longer quantized to presented frames. Each record carries the event's
// own timestamp (writer side, performance.now() milliseconds — the same
// clock lk_now_ms reports), and conversion stamps the
// InputEvent with it, so events distribute across steps by when they
// happened. If the drain thread cannot start, poll() drains per frame
// exactly as build 62 did.
//
// Activation is conditional: the ring goes live only where the page is
// cross-origin isolated and SharedArrayBuffer + Atomics exist (the same
// requirement the pthreads build already makes). Until the first ring
// record lands — and permanently, where SAB is unavailable — poll() keeps
// draining the legacy string queue below, byte-for-byte.
//
// Header words: 0 magic | 1 write index (JS-owned) | 2 read index
// (C++-owned) | 3 dropped-record count (JS-owned) | 4 cssW (float bits) |
// 5 cssH (float bits) | 6 framebuffer W | 7 framebuffer H.
// Record words (20): 0 type | 1 flags | 2 i1 | 3 spare | 4..7 float32
// fields | 8..11 code-string bytes (16, NUL-padded UTF-8) | 12..15
// key-string bytes (16) | 16..17 event timestamp (float64 milliseconds,
// lo|hi) | 18..19 reserved (zero).
// Types: 1 kd 2 ku 3 md 4 mu 5 mm 6 wh 7 plc 8 ple 9 fo 10 bl — the legacy
// queue's two-letter tags, numbered.
// ---------------------------------------------------------------------------
constexpr uint32_t kLkioMagic = 0x4C4B494Fu;  // "LKIO"
constexpr int kLkioHdrWords = 8;
constexpr int kLkioCap = 512;      // records (power of two)
constexpr int kLkioRecWords = 20;  // 80 bytes per record
constexpr int kLkioTotalWords = kLkioHdrWords + kLkioCap * kLkioRecWords;
alignas(64) std::atomic<uint32_t> g_lkioRing[kLkioTotalWords];

float lkioFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

/// Event timestamp from record words 16/17 (float64 milliseconds on the
/// performance.now clock — the same clock lk_now_ms reports),
/// in seconds. Returns 0 when the stamp is absent or implausible; the
/// caller falls back to conversion time, the pre-63 behavior.
double lkioEventSeconds(const uint32_t* rec, double nowSec) {
  const uint64_t bits = uint64_t(rec[16]) | (uint64_t(rec[17]) << 32);
  if (bits == 0) return 0.0;
  double ms = 0;
  std::memcpy(&ms, &bits, 8);
  const double sec = ms / 1000.0;
  if (!(sec > 0.0) || sec > nowSec + 0.25) return 0.0;  // NaN / future stamp: distrust
  return sec;
}

/// 16 NUL-padded bytes packed little-endian across 4 words -> C string.
void lkioStr(const uint32_t* w, char* out) {
  for (int i = 0; i < 16; ++i) out[i] = char((w[i / 4] >> ((i % 4) * 8)) & 0xFFu);
  out[16] = '\0';
}

/// HTML5 input backend for the browser window: DOM events in (via the
/// main-thread listener queue), InputEvents out — the GLFW backend's
/// contract on web primitives.
class WebHtml5InputBackend final : public IInputBackend {
 public:
  explicit WebHtml5InputBackend(std::shared_ptr<InputSharedState> st) : st_(std::move(st)) {
    // Instrumentation switch: ?debuginput=1 in the page URL (read once,
    // here, straight from the address bar the player actually used).
    // Read the flag on the browser MAIN thread: this backend runs on the
    // proxied pthread, where `location` is the worker script's URL (no page
    // query string) — a plain read here silently always saw "off".
    // The pages set window.LK_DEBUG_INPUT when ?debuginput=1 is present;
    // the main thread's own location.search is the fallback source.
    st_->debugEcho = lk_input_debug_flag() != 0;
    st_->moveEchoBudget = st_->debugEcho ? 24 : 0;
    // Install the main-thread DOM listeners (once). Keyboard on the
    // document: keys belong to whichever frame holds the browser's focus
    // (itch focuses its game iframe on click), and document delivery works
    // regardless of which element inside the frame is focused. The consume
    // rule (preventDefault set) mirrors the old callback return values.
    // The listeners themselves live in the page layer (lkInputInstall
    // in lkboot.js): the ring writer and the legacy queue are page-side
    // state, and the install is once-guarded there. Install runs before
    // the ring publish below — the ordering is load-bearing (the emit
    // path checks the ring flag the publish sets).
    lk_input_install();
    logInfo("input: web-html5 backend live (transport: main-thread listeners; ring drains at step cadence, legacy queue per frame; debug echo %s)",
            st_->debugEcho ? "ON (?debuginput=1)" : "off");
    // Build 59: publish the memory-mapped input ring to the listeners.
    // The magic word is set before the address is revealed, and the JS
    // side writes only once the publish sets window.__lkRingOn — and
    // only where the page genuinely has shared memory (cross-origin
    // isolation). A 0 return means exactly that (no shared memory);
    // the legacy queue keeps carrying, and poll() keeps draining it
    // until the first ring record lands.
    g_lkioRing[0].store(kLkioMagic, std::memory_order_relaxed);
    const int ringBase = (int)reinterpret_cast<uintptr_t>(&g_lkioRing[0]);
    ringActive_ = lk_input_ring_publish(ringBase, kLkioCap, kLkioTotalWords) != 0;
    logInfo("input: memory-mapped ring %s (records at wasm+%d; legacy queue %s)",
            ringActive_ ? "LIVE" : "unavailable (page has no shared memory) — legacy transport only", ringBase,
            ringActive_ ? "on standby until first record" : "carrying");
    // Drain fix (build 63): the ring's consumer moves OFF the frame loop.
    // A dedicated thread drains at step cadence (256 wakes/s — the web
    // step clock), so transport latency is bounded by one step period
    // even when presented frames stretch to seconds under load. If the
    // thread cannot start, poll() keeps the build-62 per-frame drain.
    if (ringActive_) {
      try {
        logInfo("threads: input mapped-ring drain thread starting");  // Q5b census naming (2026-10-28)
        drainThread_ = std::thread([this] { drainLoop(); });
        drainThreadOn_ = true;
        logInfo("input: mapped ring drain at STEP cadence (256/s, dedicated thread; frames no longer gate input)");
      } catch (...) {
        drainThreadOn_ = false;
        logError("input: step-drain thread unavailable — poll() drains the ring per frame (build 62 behavior)");
      }
    }
  }

  ~WebHtml5InputBackend() override {
    drainStop_.store(true, std::memory_order_release);
    if (drainThread_.joinable()) drainThread_.join();
  }

  std::string_view name() const override { return "web-html5"; }

  void poll(std::vector<InputEvent>& out) override {
    if (ringActive_ && !drainThreadOn_) drainRing();  // fallback only: the drain thread owns the ring when it runs
    if (!ringSeen_.load(std::memory_order_acquire)) drainJsQueue();  // legacy transport until the ring proves itself
    std::lock_guard lock(st_->mutex);
    out.insert(out.end(), st_->queue.begin(), st_->queue.end());
    st_->queue.clear();
  }

 private:
  /// Step-cadence drain loop (build 63): one drain pass per step period
  /// of the web step clock (1/256 s), resyncing instead of bursting after
  /// a stall. Runs on its own pthread; the only shared words are the ring
  /// header + records (Atomics on the JS side, std::atomic here — the
  /// same linear memory), plus the conversion state, which convMutex_
  /// serializes against the legacy drain's conversions in poll().
  void drainLoop() {
    constexpr double kStepPeriod = 1.0 / 256.0;
    double next = nowSeconds() + kStepPeriod;
    while (!drainStop_.load(std::memory_order_acquire)) {
      drainRing();
      const double now = nowSeconds();
      if (next <= now) next = now + kStepPeriod;  // fell behind: resync, never burst
      const double wait = next - now;
      next += kStepPeriod;
      if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    }
    drainRing();  // final drain: don't lose the events that landed during shutdown
  }

  /// Memory-mapped transport: consume every record the main-thread
  /// listeners have committed since the last drain, converting through
  /// the same handlers as the legacy queue. Called by the drain thread
  /// (step cadence) or, as fallback, by poll() (frame cadence).
  void drainRing() {
    std::lock_guard convLock(convMutex_);
    // Canvas geometry rides the header (refreshed by the listeners on
    // non-mousemove records and resizes); adopt it before converting, the
    // same order the legacy drain applies its leading `sz` record.
    const float cssW = lkioFloat(g_lkioRing[4].load(std::memory_order_relaxed));
    const float cssH = lkioFloat(g_lkioRing[5].load(std::memory_order_relaxed));
    if (cssW > 0 && cssH > 0) {
      st_->cssW = cssW;
      st_->cssH = cssH;
      st_->fbW = (int)g_lkioRing[6].load(std::memory_order_relaxed);
      st_->fbH = (int)g_lkioRing[7].load(std::memory_order_relaxed);
    }
    const uint32_t w = g_lkioRing[1].load(std::memory_order_acquire);
    uint32_t r = g_lkioRing[2].load(std::memory_order_relaxed);
    bool any = false;
    int guard = 0;
    while (r != w && guard++ < kLkioCap) {
      uint32_t rec[kLkioRecWords];
      const size_t base = size_t(kLkioHdrWords) + size_t(r % kLkioCap) * kLkioRecWords;
      for (int i = 0; i < kLkioRecWords; ++i) rec[i] = g_lkioRing[base + i].load(std::memory_order_relaxed);
      ++r;
      any = true;
      const uint32_t flags = rec[1];
      auto fld = [&rec](int i) { return double(lkioFloat(rec[i])); };
      // The event's own step attribution: the writer-stamped event time,
      // falling back to conversion time when the stamp is unusable.
      const double nowSec = nowSeconds();
      const double evSec = lkioEventSeconds(rec, nowSec);
      const double t = evSec > 0 ? evSec : nowSec;
      switch (rec[0]) {
        case 1:
        case 2: {
          char code[17], key[17];
          lkioStr(&rec[8], code);
          lkioStr(&rec[12], key);
          handleKeyVals(st_.get(), rec[0] == 1, code, key, (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0, t);
          break;
        }
        case 3:
        case 4: handleMouseButtonVals(st_.get(), rec[0] == 3, (int)rec[2], fld(4), fld(5), t); break;
        case 5: handleMouseMoveVals(st_.get(), fld(4), fld(5), fld(6), fld(7), t); break;
        case 6: handleWheelVals(st_.get(), fld(4), (int)rec[2], t); break;
        case 7: handlePointerLockChange(st_.get(), (flags & 1) != 0, t); break;
        case 8: handlePointerLockError(st_.get()); break;
        case 9: handleFocus(st_.get(), true, t); break;
        case 10: handleFocus(st_.get(), false, t); break;
        default: break;  // unknown record type: skip (forward-compatible)
      }
    }
    if (r != w) {
      // Index desync (cannot happen under the SPSC protocol; the writer
      // never lets w-r exceed the cap): resync to the write index rather
      // than spin, and say so once.
      if (!ringDesyncLogged_) {
        ringDesyncLogged_ = true;
        logError("input: mapped ring index desync — resynced to write index");
      }
      r = w;
      any = true;
    }
    if (any) {
      ringSeen_.store(true, std::memory_order_release);
      g_lkioRing[2].store(r, std::memory_order_release);
    }
    const uint32_t dropped = g_lkioRing[3].load(std::memory_order_relaxed);
    if (dropped != 0 && !ringDropLogged_) {
      ringDropLogged_ = true;
      logError("input: mapped ring overflow — %u input record(s) dropped (ring full)", dropped);
    }
  }

  /// One main-thread hop per frame: fetch + clear the page-side event
  /// queue and the canvas geometry, then convert every record into
  /// InputEvents.
  void drainJsQueue() {
    if (drainBuf_.empty()) drainBuf_.resize(1 << 16);
    const int n = lk_input_drain_queue(drainBuf_.data(), (int)drainBuf_.size());
    if (n < 0) {
      // The pending records do not fit the drain buffer. The page side
      // now leaves the queue intact on overflow (B3 fix, 2026-10-28), so
      // the drain retries next frame instead of dropping the records;
      // a queue that stays oversized logs here per attempt rather than
      // wedging shut.
      logError("input: drain buffer overflow — input records retained for retry");
      return;
    }
    if (n == 0) return;
    // Conversions serialize with the ring drain thread's (build 63): both
    // feed the same handlers and conversion state. The bridge hop above
    // deliberately stays outside this lock.
    std::lock_guard convLock(convMutex_);
    const std::string_view text(drainBuf_.data(), (size_t)n);
    size_t b = 0;
    while (b < text.size()) {
      const size_t e = text.find('\n', b);
      const std::string_view line = text.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b);
      b = (e == std::string_view::npos) ? text.size() : e + 1;
      if (line.empty()) continue;
      const auto f = splitFields(line);
      if (f.empty()) continue;
      if (f[0] == "sz") {
        st_->cssW = fieldNum(f, 1); st_->cssH = fieldNum(f, 2);
        st_->fbW = (int)fieldNum(f, 3); st_->fbH = (int)fieldNum(f, 4);
      } else if (f[0] == "kd") handleKey(st_.get(), true, f);
      else if (f[0] == "ku") handleKey(st_.get(), false, f);
      else if (f[0] == "md") handleMouseButton(st_.get(), true, f);
      else if (f[0] == "mu") handleMouseButton(st_.get(), false, f);
      else if (f[0] == "mm") handleMouseMove(st_.get(), f);
      else if (f[0] == "wh") handleWheel(st_.get(), f);
      else if (f[0] == "plc") handlePointerLockChange(st_.get(), fieldFlag(f, 1), nowSeconds());
      else if (f[0] == "ple") handlePointerLockError(st_.get());
      else if (f[0] == "fo") handleFocus(st_.get(), true, nowSeconds());
      else if (f[0] == "bl") handleFocus(st_.get(), false, nowSeconds());
    }
  }

  std::shared_ptr<InputSharedState> st_;
  std::vector<char> drainBuf_;
  bool ringActive_ = false;            // SAB + Atomics present; ring published to the listeners
  std::atomic<bool> ringSeen_{false};  // first ring record consumed (legacy drain retires)
  bool ringDropLogged_ = false;        // drain-side only, under convMutex_
  bool ringDesyncLogged_ = false;      // drain-side only, under convMutex_
  std::mutex convMutex_;               // serializes ALL conversions (ring thread + legacy drain)
  std::thread drainThread_;            // build 63: step-cadence ring consumer
  std::atomic<bool> drainStop_{false};
  bool drainThreadOn_ = false;
};

}  // namespace

struct Window::Impl {
  std::shared_ptr<InputSharedState> inputState = std::make_shared<InputSharedState>();
  std::unique_ptr<IInputBackend> input = std::make_unique<WebHtml5InputBackend>(inputState);
  std::string clipboard;  // page-session clipboard only (not the OS clipboard)
  bool closeRequested = false;
  int fbW = 0, fbH = 0;
  int glCtx = 0;  // OpenGL33 windows only (page-layer context handle)
};

std::unique_ptr<Window> Window::create(const WindowDesc& d) {
  auto w = std::make_unique<Window>(Passkey{});
  w->api_ = d.api;
  w->impl_ = std::make_unique<Impl>();
  w->vsync_ = d.vsync;
  w->winW_ = d.width;
  w->winH_ = d.height;
  w->display_ = d.display;
  w->impl_->fbW = d.width;
  w->impl_->fbH = d.height;
  // Size the page canvas to the requested window. Failure is non-fatal: the
  // shell page may manage the canvas itself; framebufferSize() then reports
  // the desc size until the element exists.
  lk_canvas_set_size(d.width, d.height);
  logInfo("window: web window %dx%d (api %s)", d.width, d.height,
          d.api == GraphicsApi::Vulkan ? "vulkan" : "gl33");
  if (d.api == GraphicsApi::OpenGL33) {
    LkGlContextAttrs attrs;
    lk_gl_attrs_init(&attrs);
    attrs.alpha = false;
    attrs.depth = true;
    attrs.stencil = true;
    // Pacing pass (2026-10-19): ?aa=on|4 in the page URL requests a
    // multisampled default framebuffer. WebGL's context attribute is a
    // boolean — ANGLE's implicit MSAA is conventionally 4x, so "4" and
    // "on" land in the same place; anything else (including no param)
    // keeps the shipped default OFF. Read on the browser MAIN thread:
    // this code runs on the proxied pthread, whose `location` is the
    // worker script's URL, not the page's (the ?debuginput trap).
    const int aaMode = lk_page_aa_mode();
    attrs.antialias = aaMode != 0;
    logInfo("window: WebGL antialias %s (?aa=%s)", aaMode ? "requested" : "off",
            aaMode == 1 ? "on" : aaMode == 4 ? "4" : "off");
    // Test-build choice: keep the last presented frame in the drawing
    // buffer so it stays inspectable after main() returns (a screenshot
    // taken post-exit must not race the compositor's buffer clear).
    attrs.preserveDrawingBuffer = true;
    attrs.majorVersion = 2;  // WebGL2 == GLES3, the GL33 backend's dialect
    attrs.minorVersion = 0;
    attrs.enableExtensionsByDefault = true;
    const int ctx = lk_gl_create_context(&attrs);
    if (ctx > 0) {
      w->impl_->glCtx = ctx;
      lk_gl_make_current(ctx);
      logInfo("window: WebGL2 context current on #canvas");
    } else {
      logError("window: WebGL2 context creation failed — GL backends will fail their own init gates");
    }
  }
  return w;
}

const char* displayModeName(DisplayMode m) {
  switch (m) {
    case DisplayMode::Windowed: return "windowed";
    case DisplayMode::Maximized: return "maximized";
    case DisplayMode::Borderless: return "borderless";
    case DisplayMode::Fullscreen: return "fullscreen";
  }
  return "windowed";
}

bool parseDisplayMode(const std::string& s, DisplayMode& out) {
  for (DisplayMode m : {DisplayMode::Windowed, DisplayMode::Maximized, DisplayMode::Borderless, DisplayMode::Fullscreen})
    if (s == displayModeName(m)) { out = m; return true; }
  return false;
}

void Window::setDisplay(const DisplayDesc& d) {
  display_ = d;
  if (d.mode != DisplayMode::Windowed) lastFull_ = d.mode;
  if (d.width > 0 && d.height > 0) {
    winW_ = d.width;
    winH_ = d.height;
    impl_->fbW = d.width;
    impl_->fbH = d.height;
    lk_canvas_set_size(d.width, d.height);
  }
}

bool Window::pollDisplayToggle() { return false; }  // no Alt+Enter on the web

Window::Window(Passkey) {}
Window::~Window() = default;

bool Window::shouldClose() const { return impl_->closeRequested; }
bool Window::focused() const { return impl_->inputState->focused.load(); }
void Window::requestClose() { impl_->closeRequested = true; }

IInputBackend& Window::input() { return *impl_->input; }
void Window::raiseAndFocus() {}
void Window::setTopmost(bool on) { topmost_ = on; }  // no window manager; flag only
bool Window::cursorReallyCaptured() const { return impl_->inputState->pointerLocked.load(); }
void Window::takeFocusEdges(int& lost, int& gained) {
  lost = impl_->inputState->focusLostEdges.exchange(0);
  gained = impl_->inputState->focusGainedEdges.exchange(0);
}

bool Window::loadGamepadMappings(const std::string&) { return false; }

void Window::swap() {}  // the page compositor presents the canvas; see header note

void Window::setTitle(const std::string& t) {
  // Game code runs on the proxied pthread, which has no DOM: the write
  // goes through the named import, which the page layer executes on
  // the browser main thread.
  lk_window_set_title(t.c_str());
}

std::string Window::clipboardText() const { return impl_->clipboard; }
void Window::setClipboardText(const std::string& text) { impl_->clipboard = text; }

void Window::setCursorCaptured(bool c) {
  captured_ = c;
  InputSharedState& st = *impl_->inputState;
  st.captureWanted = c;
  // Mirror the want into the main-thread listener state (transitions
  // only — this is re-applied every frame): the JS mousedown handler is
  // the reliable pointer-lock requester, and it reads this flag.
  static bool jsCaptureSent = false;
  if (c != jsCaptureSent) {
    jsCaptureSent = c;
    lk_input_set_capture_wanted(c ? 1 : 0);
  }
  if (c) {
    // Pointer lock is the web's cursor capture. The game loop re-applies
    // this every frame (native parity), so throttle hard: a refused
    // request must not spam the console at frame rate. The reliable path
    // is the mousedown handler, which re-requests inside the gesture.
    // Drain-fix rider: Chrome additionally REJECTS a re-lock requested
    // within ~1.25 s of the user exiting lock (an uncaught SecurityError
    // in the page console — JD's build-62 console dump). Inside that
    // cooldown window the frame loop does not ask at all; the first
    // request after it (this loop's, or the next click's) lands cleanly.
    if (!st.pointerLocked.load()) {
      const double now = nowSeconds();
      const double rel = st.lastLockRelease.load();
      if (!(rel > 0 && now - rel < 1.3)) {
        double last = st.lastLockRequest.load();
        if (now - last > 0.5) {
          st.lastLockRequest = now;
          lk_pointer_lock_request();
        }
      }
    }
  } else if (st.pointerLocked.load()) {
    lk_pointer_lock_exit();
  }
}

void Window::framebufferSize(int& w, int& h) const {
  int cw = 0, ch = 0;
  if (lk_canvas_get_size(&cw, &ch) == 0 && cw > 0 && ch > 0) {
    w = cw;
    h = ch;
  } else {
    w = impl_->fbW;
    h = impl_->fbH;
  }
}

void* Window::glProcAddress(const char* name) { return lk_gl_proc_address(name); }

std::vector<std::string> Window::vulkanInstanceExtensions() const { return {}; }
uint64_t Window::createVulkanSurface(void*) const { return 0; }

void Window::setIcon(const IconPixels*, int) {}  // no window icon in a browser tab

}  // namespace litholattice

// ---- context attribute defaults + the GL address table ----------------
// Defined at global scope: lk_gl_attrs_init and lk_gl_proc_address are
// part of the in-house web surface declared in lk_web.h.

void lk_gl_attrs_init(LkGlContextAttrs* attrs) {
  // The shipped defaults, matching what the page layer's context
  // creation has always received from this TU: zeroed, then alpha,
  // depth, antialias, premultiplied alpha, major version 1 and
  // extensions-by-default on. Window::create overrides from here.
  *attrs = LkGlContextAttrs{};
  attrs->alpha = true;
  attrs->depth = true;
  attrs->antialias = true;
  attrs->premultipliedAlpha = true;
  attrs->majorVersion = 1;
  attrs->enableExtensionsByDefault = true;
  // Engine code never runs on the browser main thread (the window is
  // created from an engine thread), so the proxy mode takes the
  // fallback value previous builds produced on every real path. The
  // page layer streams context creation to the page regardless and
  // does not consult the field.
  attrs->proxyContextToMainThread = 1;
}

namespace {
struct LkGlProcEntry {
  const char* name;
  void* addr;
};
const LkGlProcEntry kLkGlProcs[] = {
#define LK_GL_PROC_ENTRY(ret, name, args) { "gl" #name, reinterpret_cast<void*>(&lk_gl##name) },
    LITHOLATTICE_GL_FUNCS(LK_GL_PROC_ENTRY)
#undef LK_GL_PROC_ENTRY
};
}  // namespace

void* lk_gl_proc_address(const char* name) {
  if (!name) return nullptr;
  // WebGL2 has no buffer textures (TexBuffer) and no core base-vertex
  // draw (DrawElementsBaseVertex): answering null routes both names
  // to the loader fixups, which install the in-house shims
  // (gl_web_shims.cpp) — the resolution those names have always had
  // on web, expected-log line included.
  if (!std::strcmp(name, "glTexBuffer") || !std::strcmp(name, "glDrawElementsBaseVertex")) return nullptr;
  for (const LkGlProcEntry& e : kLkGlProcs)
    if (!std::strcmp(name, e.name)) return e.addr;
  return nullptr;
}
