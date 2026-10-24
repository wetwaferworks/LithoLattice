// cDialog (shown modally with cUiRoot::pushModal): title bar (drag to move), a body for content
// and an optional button row. Escape or close() emits "close" and removes it. Tooltip: the
// floating hint cUiRoot draws for a hovered widget's tooltip(); usable standalone too.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include "litholattice/ui/containers.h"
#include "litholattice/ui/controls.h"

namespace litholattice::ui {

class cUiRoot;

class cDialog : public cWidget {
 public:
  explicit cDialog(std::string title, Vec2 size = {});
  /// Shows a new dialog centred on screen as a modal; returns it.
  static cDialog& show(cUiRoot& ui, std::string title, Vec2 size = {});
  const std::string& title() const { return title_; }
  /// Where content goes (a vertical stack).
  cStackPanel& body() { return *body_; }
  /// Adds a button to the bottom row; `closes` closes the dialog after `onClick`.
  cButton& addButton(std::string text, std::function<void()> onClick = {}, bool closes = true);
  /// Emits "close" and removes the dialog from its cUiRoot.
  void close();
  /// Consulted when the user closes the dialog (Escape); return false to keep it open (e.g. to
  /// ask about unsaved changes first). close() itself always closes.
  void setCloseGuard(std::function<bool()> guard) { closeGuard_ = std::move(guard); }
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void arrange(const Theme& theme) override;
  void paint(Painter& p) const override;

 private:
  float titleHeight() const;
  std::string title_;
  cStackPanel* body_ = nullptr;
  cStackPanel* buttons_ = nullptr;
  Vec2 grab_{-1, -1};
  std::function<bool()> closeGuard_;
};

/// A boxed one-line hint.
class cTooltip : public cWidget {
 public:
  explicit cTooltip(std::string text = {}) : cWidget("tooltip"), text_(std::move(text)) {}
  void setText(std::string t) { text_ = std::move(t); }
  const std::string& text() const { return text_; }
  Vec2 measure(const Theme& theme) const override;
  cWidget* hitTest(Vec2) override { return nullptr; }

 protected:
  void paint(Painter& p) const override;

 private:
  std::string text_;
};

}  // namespace litholattice::ui
