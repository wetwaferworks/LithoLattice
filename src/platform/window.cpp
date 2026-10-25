// Cortana 2026-10-15 0935/1045: ALT-TAB, LAUNCH-FOCUS
#include "litholattice/platform/window.h"
#include <vector>
// Vulkan types (no prototypes) first, so GLFW declares its surface helpers.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <cstdlib>
#include <cstring>
#include "dxgi_present.h"
#endif
#include <fstream>
#include <sstream>
#include "glfw_input_backend.h"
#include "litholattice/core/log.h"

namespace litholattice {

struct Window::Impl {
  GLFWwindow* win = nullptr;
  std::unique_ptr<cGlfwInputBackend> input;
#ifdef _WIN32
  std::unique_ptr<cDxgiPresenter> dxgi;  // LL_SW_PRESENT=dxgi (2026-10-25); GDI otherwise / on failure
  bool dxgiTried = false;
#endif
};

std::unique_ptr<Window> Window::create(const WindowDesc& d) {
  if (!glfwInit()) { logError("glfwInit failed"); return nullptr; }
  const bool vulkan = d.api == GraphicsApi::Vulkan;
  glfwDefaultWindowHints();
  if (vulkan) {
    if (!glfwVulkanSupported()) { logError("GLFW: no Vulkan loader / ICD found"); glfwTerminate(); return nullptr; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  } else if (d.api == GraphicsApi::None) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  } else {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  }
  GLFWwindow* gw = glfwCreateWindow(d.width, d.height, d.title.c_str(), nullptr, nullptr);
  if (!gw) {
    logError(vulkan ? "window creation (Vulkan, no API) failed" : "window / GL 3.3 context creation failed");
    glfwTerminate();
    return nullptr;
  }
  if (d.api == GraphicsApi::OpenGL33) {
    glfwMakeContextCurrent(gw);
    glfwSwapInterval(d.vsync ? 1 : 0);
  }
  auto w = std::make_unique<Window>(Passkey{});
  w->api_ = d.api;
  w->impl_ = std::make_unique<Impl>();
  w->impl_->win = gw;
  w->impl_->input = std::make_unique<cGlfwInputBackend>(gw);
  w->vsync_ = d.vsync;
  w->winW_ = d.width; w->winH_ = d.height;
  glfwGetWindowPos(gw, &w->winX_, &w->winY_);
  w->setDisplay(d.display);
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

void Window::setDisplay(const DisplayDesc& want) {
  GLFWwindow* win = impl_->win;
  int count = 0;
  GLFWmonitor** mons = glfwGetMonitors(&count);
  if (!mons || count == 0) { logError("display: no monitor found; staying windowed"); return; }
  DisplayDesc d = want;
  if (d.monitor < 0 || d.monitor >= count) {
    logError("display: monitor %d not found (%d connected); using 0", d.monitor, count);
    d.monitor = 0;
  }
  GLFWmonitor* mon = mons[d.monitor];
  const GLFWvidmode* desk = glfwGetVideoMode(mon);
  // Leaving Windowed: remember where the window was, to restore it later.
  if (display_.mode == DisplayMode::Windowed && !glfwGetWindowMonitor(win) && glfwGetWindowAttrib(win, GLFW_DECORATED)) {
    glfwGetWindowPos(win, &winX_, &winY_);
    glfwGetWindowSize(win, &winW_, &winH_);
  }
  if (glfwGetWindowAttrib(win, GLFW_MAXIMIZED)) glfwRestoreWindow(win);
  int mx = 0, my = 0;
  glfwGetMonitorPos(mon, &mx, &my);
  switch (d.mode) {
    case DisplayMode::Windowed: {
      if (d.width > 0 && d.height > 0) { winW_ = d.width; winH_ = d.height; }
      glfwSetWindowAttrib(win, GLFW_DECORATED, GLFW_TRUE);
      glfwSetWindowMonitor(win, nullptr, winX_, winY_, winW_, winH_, 0);
      break;
    }
    case DisplayMode::Maximized:
      glfwSetWindowAttrib(win, GLFW_DECORATED, GLFW_TRUE);
      glfwSetWindowMonitor(win, nullptr, mx + 50, my + 50, winW_, winH_, 0);
      glfwMaximizeWindow(win);
      break;
    case DisplayMode::Borderless:
      // 2026-10-25 fix pass (JD J2a, option b): GLFW's windowed-full-screen idiom. Claiming the monitor at its
      // own desktop mode gives a borderless window over the taskbar with no mode switch; the old frameless
      // desktop-sized window could keep a frame and leave the taskbar on top. No auto-iconify, so Alt+Tab
      // leaves it in place.
      glfwSetWindowAttrib(win, GLFW_AUTO_ICONIFY, GLFW_FALSE);
      if (api_ != GraphicsApi::Vulkan) {
        // 2026-10-25 (JD, capture fix): a monitor-owned GL window is presented by the driver
        // outside DWM composition (exclusive flip), so Print Screen, Snipping Tool and every
        // OBS capture mode see a blank surface on the software / gl33 path. Use an undecorated,
        // monitor-sized, non-monitor-owned window instead: same look, composited by DWM, capturable.
        // Vulkan keeps the monitor idiom (its swapchain stays capturable).
        if (glfwGetWindowMonitor(win)) glfwSetWindowMonitor(win, nullptr, mx, my, desk->width, desk->height, 0);
        glfwSetWindowAttrib(win, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowMonitor(win, nullptr, mx, my, desk->width, desk->height, 0);
      } else {
        glfwSetWindowMonitor(win, mon, 0, 0, desk->width, desk->height, desk->refreshRate);
      }
      break;
    case DisplayMode::Fullscreen: {
      glfwSetWindowAttrib(win, GLFW_AUTO_ICONIFY, GLFW_TRUE);  // GLFW default for exclusive full screen
      const int w = d.width > 0 ? d.width : desk->width, h = d.height > 0 ? d.height : desk->height;
      int n = 0, hz = 0;
      const GLFWvidmode* modes = glfwGetVideoModes(mon, &n);
      for (int i = 0; i < n; ++i)
        if (modes[i].width == w && modes[i].height == h && (d.refreshHz > 0 ? modes[i].refreshRate == d.refreshHz : modes[i].refreshRate > hz))
          hz = modes[i].refreshRate;
      if (hz == 0) {
        logError("display: %dx%d@%d not offered by monitor %d; using its desktop mode", w, h, d.refreshHz, d.monitor);
        glfwSetWindowMonitor(win, mon, 0, 0, desk->width, desk->height, desk->refreshRate);
      } else {
        glfwSetWindowMonitor(win, mon, 0, 0, w, h, hz);
      }
      break;
    }
  }
  if (api_ == GraphicsApi::OpenGL33) glfwSwapInterval(vsync_ ? 1 : 0);  // some drivers reset it on a mode change
  if (d.mode != DisplayMode::Windowed) lastFull_ = d.mode;
  display_ = d;
  int fw = 0, fh = 0;
  glfwGetFramebufferSize(win, &fw, &fh);
  const GLFWmonitor* cur = glfwGetWindowMonitor(win);
  const GLFWvidmode* vm = glfwGetVideoMode(cur ? const_cast<GLFWmonitor*>(cur) : mon);
  logInfo("display: %s on monitor %d (%s), framebuffer %dx%d, monitor mode %dx%d@%d Hz, vsync %s", displayModeName(d.mode), d.monitor,
          glfwGetMonitorName(mon), fw, fh, vm->width, vm->height, vm->refreshRate, vsync_ ? "on" : "off");
}

bool Window::pollDisplayToggle() {
  GLFWwindow* win = impl_->win;
  const bool alt = glfwGetKey(win, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
  const bool down = alt && (glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_KP_ENTER) == GLFW_PRESS);
  const bool fire = down && !toggleHeld_;
  toggleHeld_ = down;
  if (!fire) return false;
  DisplayDesc d = display_;
  d.mode = display_.mode == DisplayMode::Windowed ? lastFull_ : DisplayMode::Windowed;
  setDisplay(d);
  return true;
}

Window::Window(Passkey) {}
Window::~Window() {
#ifdef _WIN32
  if (impl_) impl_->dxgi.reset();
#endif
  if (impl_) impl_->input.reset();  // unhooks its callbacks before the window goes
  if (impl_ && impl_->win) glfwDestroyWindow(impl_->win);
  glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(impl_->win); }
bool Window::focused() const { return glfwGetWindowAttrib(impl_->win, GLFW_FOCUSED) == GLFW_TRUE; }
void Window::requestClose() { glfwSetWindowShouldClose(impl_->win, GLFW_TRUE); }

IInputBackend& Window::input() { return *impl_->input; }
void Window::raiseAndFocus() {
  glfwShowWindow(impl_->win);
  glfwFocusWindow(impl_->win);  // Win32: SetForegroundWindow-based; may only flash the taskbar
  if (glfwGetWindowAttrib(impl_->win, GLFW_FOCUSED) != GLFW_TRUE) {
    // Windows foreground-lock fallback: a brief topmost toggle raises the
    // window above the app that has focus; then ask for focus again.
    glfwSetWindowAttrib(impl_->win, GLFW_FLOATING, GLFW_TRUE);
    glfwSetWindowAttrib(impl_->win, GLFW_FLOATING, topmost_ ? GLFW_TRUE : GLFW_FALSE);
    glfwFocusWindow(impl_->win);
  }
}
void Window::setTopmost(bool on) {
  if (on == topmost_) return;
  topmost_ = on;
  glfwSetWindowAttrib(impl_->win, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE);
}
bool Window::cursorReallyCaptured() const { return glfwGetInputMode(impl_->win, GLFW_CURSOR) == GLFW_CURSOR_DISABLED; }
void Window::takeFocusEdges(int& lost, int& gained) { impl_->input->takeFocusEdges(lost, gained); }

bool Window::loadGamepadMappings(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  const bool ok = impl_->input->addGamepadMappings(ss.str().c_str());
  logInfo("gamepad mappings %s: %s", path.c_str(), ok ? "loaded" : "rejected");
  return ok;
}

void Window::swap() {
  if (api_ == GraphicsApi::OpenGL33) glfwSwapBuffers(impl_->win);
}
void Window::setTitle(const std::string& t) { glfwSetWindowTitle(impl_->win, t.c_str()); }
std::string Window::clipboardText() const {
  const char* s = glfwGetClipboardString(impl_->win);
  return s ? s : "";
}
void Window::setClipboardText(const std::string& text) { glfwSetClipboardString(impl_->win, text.c_str()); }
void Window::setCursorCaptured(bool c) {
  captured_ = c;
  impl_->input->resetMouseBaseline();
  glfwSetInputMode(impl_->win, GLFW_CURSOR, c ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (!c) showCursorNow();
}
// 2026-10-25 (JD, alt-tab cursor): after an Alt+Tab round trip GLFW can return to
// NORMAL mode without re-setting the arrow (its "cursor in content area" flag is
// stale), leaving an invisible cursor over the menus. Re-assert the default arrow
// and, on Windows, the ShowCursor display count. Shared by every renderer.
bool Window::presentPixels(const uint32_t* pixels, int w, int h) {
#ifdef _WIN32
  if (!pixels || w <= 0 || h <= 0) return false;
  HWND hwnd = glfwGetWin32Window(impl_->win);
  if (!hwnd) return false;
  if (!impl_->dxgiTried) {
    impl_->dxgiTried = true;
    const char* e = std::getenv("LL_SW_PRESENT");
    // 2026-10-25: DXGI flip-discard is the default; LL_SW_PRESENT=gdi forces GDI (and any DXGI
    // failure falls back to GDI automatically).
    if (!(e && std::strcmp(e, "gdi") == 0)) impl_->dxgi = cDxgiPresenter::create(hwnd);
    if (!impl_->dxgi) logInfo("software present: GDI StretchDIBits");
  }
  if (impl_->dxgi) {
    if (IsIconic(hwnd)) return true;
    if (impl_->dxgi->present(pixels, w, h, vsync_)) return true;
    logError("dxgi present failed; falling back to GDI");
    impl_->dxgi.reset();
  }
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0) return true;  // minimised
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top row first
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;  // 0xAARRGGBB little-endian = B,G,R,A bytes = GDI's 32bpp BI_RGB
  bi.bmiHeader.biCompression = BI_RGB;
  HDC dc = GetDC(hwnd);
  if (!dc) return false;
  SetStretchBltMode(dc, COLORONCOLOR);
  const int rows = StretchDIBits(dc, 0, 0, cw, ch, 0, 0, w, h, pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(hwnd, dc);
  ValidateRect(hwnd, nullptr);  // the frame is the paint; no WM_PAINT erase over it
  if (vsync_) DwmFlush();       // pace to the compositor (GDI has no swap interval)
  return rows != 0;
#else
  (void)pixels; (void)w; (void)h;
  return false;
#endif
}
void Window::showCursorNow() {
  glfwSetCursor(impl_->win, nullptr);
#ifdef _WIN32
  for (int i = 0; i < 16 && ShowCursor(TRUE) < 0; ++i) {}
  POINT pt;
  if (GetCursorPos(&pt)) SetCursorPos(pt.x, pt.y);  // a synthetic move makes Windows re-run WM_SETCURSOR
#endif
}
void Window::framebufferSize(int& w, int& h) const { glfwGetFramebufferSize(impl_->win, &w, &h); }
void* Window::glProcAddress(const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); }

std::vector<std::string> Window::vulkanInstanceExtensions() const {
  uint32_t count = 0;
  const char** names = glfwGetRequiredInstanceExtensions(&count);
  return names ? std::vector<std::string>(names, names + count) : std::vector<std::string>{};
}

uint64_t Window::createVulkanSurface(void* instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(static_cast<VkInstance>(instance), impl_->win, nullptr, &surface) != VK_SUCCESS)
    return 0;
  return reinterpret_cast<uint64_t>(surface);
}

void Window::setIcon(const IconPixels* images, int count) {
  if (!impl_->win || count <= 0) return;
  std::vector<GLFWimage> imgs;
  for (int k = 0; k < count; ++k)
    if (images[k].rgba && images[k].width > 0 && images[k].height > 0)
      imgs.push_back({images[k].width, images[k].height, const_cast<unsigned char*>(images[k].rgba)});
  if (!imgs.empty()) glfwSetWindowIcon(impl_->win, int(imgs.size()), imgs.data());
}

}  // namespace litholattice
