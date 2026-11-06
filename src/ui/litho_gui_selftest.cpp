// litho_gui_selftest.cpp: compact integration checks for Litho GUI in-engine.
#include "litholattice/ui/litho_gui_selftest.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "litholattice/ui/litho_gui.h"

namespace litholattice {
namespace {

// The Trace default theme (assets/ui/trace-default.json ships this text; the
// selftest carries a representative excerpt so it needs no file IO).
const char* kThemeExcerpt = R"JSON({
  "uiScale": 1.0,
  "opacity": { "global": 1.0, "trace": 1.0 },
  "trace": { "defaultPhase": 0.0, "phaseSpeed": 1.0 },
  "shaders": { "trace-border": "shaders/ui/trace-border.glsl",
               "panel-fill": "shaders/ui/panel-fill.glsl" }
})JSON";

}  // namespace

bool runLithoGuiSelftest() {
  int fails = 0;
  const auto check = [&fails](bool ok, const char* what) {
    std::printf("  [%s] ui: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };

  lithogui::cLithoGui gui;

  // Small tree: Frame (engaged, design space) -> Window -> Label + Button.
  auto frame = std::make_unique<lithogui::Frame>("selftest-frame", 1280.0f, 720.0f);
  auto window = std::make_unique<lithogui::Window>("selftest-window", "Selftest");
  window->dir = lithogui::LayoutDir::Column;
  window->setFrame({40.0f, 40.0f, 400.0f, 200.0f});
  auto label = std::make_unique<lithogui::Label>("selftest-label", "Hello Litho GUI");
  label->fixedH = 24.0f;
  window->addChild(std::move(label));
  auto button = std::make_unique<lithogui::Button>("selftest-button", "Press");
  button->fixedH = 32.0f;
  bool clicked = false;
  button->onClick = [&clicked](lithogui::Button&) { clicked = true; };
  lithogui::Button* buttonPtr = button.get();
  window->addChild(std::move(button));
  frame->addChild(std::move(window));
  lithogui::Frame* framePtr = frame.get();
  gui.setRoot(framePtr);  // the GUI does not own the tree; `frame` keeps it alive

  gui.layout({0.0f, 0.0f, 1280.0f, 720.0f});
  const lithogui::Rect br = buttonPtr->rect();
  check(br.w > 0.0f && br.h > 0.0f, "layout gives the button a real rect at 1280x720");

  // treeRect maps the button into tree (physical at uiScale 1) coordinates.
  const lithogui::Rect tr = buttonPtr->treeRect();
  const float cx = tr.x + tr.w * 0.5f;
  const float cy = tr.y + tr.h * 0.5f;
  lithogui::PointerEvent down;
  down.type = lithogui::PointerEvent::Type::Down;
  down.x = cx;
  down.y = cy;
  down.button = 0;
  gui.dispatchPointer(down);
  lithogui::PointerEvent up = down;
  up.type = lithogui::PointerEvent::Type::Up;
  gui.dispatchPointer(up);
  check(clicked, "pointer click on the button fires its onClick callback");

  lithogui::DrawList dl;
  gui.emitFrame(dl, 0.0f);
  check(!dl.commands().empty(), "emitFrame produces a non-empty draw list");

  std::vector<std::string> errors;
  const bool themeOk = gui.applyThemeJson(kThemeExcerpt, &errors);
  check(themeOk && errors.empty(), "applyThemeJson parses the Trace theme text");

  gui.setRoot(nullptr);
  return fails == 0;
}

}  // namespace litholattice
