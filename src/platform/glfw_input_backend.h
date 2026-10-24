// Cortana 2026-10-15 0935: ALT-TAB focus edges
// Private to litholattice_platform: GLFW implementation of IInputBackend.
#pragma once
#include <array>
#include <vector>
#include "litholattice/input/input_backend.h"

struct GLFWwindow;

namespace litholattice {

/// Keyboard, mouse (buttons, wheel, motion, cursor), text (char callback), focus, and gamepads
/// through glfwGetGamepadState (SDL_GameControllerDB mappings built into GLFW, plus an optional
/// gamecontrollerdb.txt). Callbacks append to a pending list; poll() pumps GLFW and hands it over.
class cGlfwInputBackend final : public IInputBackend {
 public:
  explicit cGlfwInputBackend(GLFWwindow* window);
  ~cGlfwInputBackend() override;
  cGlfwInputBackend(const cGlfwInputBackend&) = delete;
  cGlfwInputBackend& operator=(const cGlfwInputBackend&) = delete;

  std::string_view name() const override { return "glfw"; }
  void poll(std::vector<InputEvent>& out) override;

  /// The next cursor sample only sets the baseline (no jump when the cursor is captured / released).
  void resetMouseBaseline() { haveCursor_ = false; }
  /// Focus edges seen by the GLFW focus callback since the last call
  /// (2026-10-15: lets the frame loop react to every alt-tab, even a quick
  /// away-and-back between two frames at low FPS). Resets the counters.
  void takeFocusEdges(int& lost, int& gained) { lost = focusLost_; gained = focusGained_; focusLost_ = focusGained_ = 0; }
  /// Adds SDL-format gamepad mappings (e.g. config/gamecontrollerdb.txt). False if rejected.
  bool addGamepadMappings(const char* text);

 private:
  struct PadState {
    bool present = false;
    std::array<unsigned char, 15> buttons{};
    std::array<float, 6> axes{};
  };
  void pollGamepads(double now);
  void push(const InputEvent& e) { pending_.push_back(e); }

  static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods);
  static void onChar(GLFWwindow* w, unsigned int codepoint);
  static void onMouseButton(GLFWwindow* w, int button, int action, int mods);
  static void onScroll(GLFWwindow* w, double dx, double dy);
  static void onCursor(GLFWwindow* w, double x, double y);
  static void onFocus(GLFWwindow* w, int focused);
  static cGlfwInputBackend* from(GLFWwindow* w);

  GLFWwindow* window_;
  std::vector<InputEvent> pending_;
  std::array<PadState, kMaxGamepads> pads_{};
  double lastX_ = 0, lastY_ = 0;
  bool haveCursor_ = false;
  int focusLost_ = 0, focusGained_ = 0;
};

}  // namespace litholattice
