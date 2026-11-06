// cDebugConsole: the Half-Life-style dropdown console for lithoscript. Tilde
// drops it from the top of the screen; it renders through Litho GUI (a
// lithogui::Panel holding a lithogui::ListBox log view over a
// lithogui::TextField command line, with a second ListBox as the completion
// dropdown — composition, no new widgets).
//
// INPUT CONFINEMENT (hard rule): the tilde key never toggles the console open
// while another text field wants the keyboard — the ILithoGui has a focused
// widget that is not this console's input, or a popup/modal is open on its
// context — so typing "~" in a field keeps working. While the console is open
// its TextField holds focus, so keystrokes typed here are the GUI's, exactly
// like any other text field. Tilde while open closes it (HL1 behavior).
//
// GAME WIRING (one call site, in the game's UI input route):
//   for (const lithogui::KeyEvent& e : keyEvents) {
//     if (debugConsole.handleTildeKey(gui, e)) continue;   // tilde toggle
//     if (debugConsole.isOpen() && debugConsole.handleKeyPreFilter(e)) continue;
//     ... gui.dispatchKey(e) ...
//   }
// plus debugConsole.update() once per frame. attach(gui) once, after the
// ILithoGui exists: the panel joins the GUI's root tree when one is set, and
// becomes the root when none is.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "litholattice/console/completer.h"
#include "litholattice/console/concmd.h"
#include "litholattice/ui/litho_gui.h"

namespace litholattice::console {

class cDebugConsole {
 public:
  explicit cDebugConsole(cConsoleReg& registry);
  ~cDebugConsole() = default;
  cDebugConsole(const cDebugConsole&) = delete;
  cDebugConsole& operator=(const cDebugConsole&) = delete;

  /// Builds the panel (hidden) and adds it to the GUI. Call once, after the
  /// ILithoGui exists. Available in ALL builds, not just debug.
  void attach(lithogui::ILithoGui& gui);

  bool isOpen() const { return open_; }
  void open();
  void close();
  void toggle();

  /// Tilde handling for the game loop. Consumes a key-down event whose
  /// character is '~' or '`' (the grave-accent key with/without shift):
  /// opens the console when closed and closes it when open. Never opens
  /// while another text field / a modal wants the keyboard (the GUI's
  /// focused widget / popup state) — that check runs before the key-repeat
  /// debounce, so typing "~" in a field always keeps working. Debounced:
  /// a held tilde key toggles at most once per 200 ms. Returns true when
  /// the event was consumed.
  bool handleTildeKey(lithogui::ILithoGui& gui, const lithogui::KeyEvent& e);

  /// Key pre-filter while open; call BEFORE gui.dispatchKey. Tab performs
  /// HL1-style completion of the input line; typing filters the VS
  /// Code-style autocomplete dropdown (a ListBox above the input) — the
  /// live refresh itself runs in update(), which polls the input text.
  /// While the dropdown is visible Up/Down move its selection, Enter
  /// accepts it, Escape dismisses it (the console stays open); otherwise
  /// Up/Down walk command history and Escape closes.
  /// Returns true when the event was consumed.
  bool handleKeyPreFilter(const lithogui::KeyEvent& e);

  /// Pulls new engine-log tap records into the log view and refreshes the
  /// completion dropdown when the input text changed (call once per frame).
  void update();

  // ---- test seams ----
  size_t historySize() const { return history_.size(); }
  std::string inputText() const;
  size_t logLineCount() const;
  /// Feeds a line as if submitted from the input box (history + dispatch).
  void submitForTest(const std::string& line);
  /// Autocomplete dropdown state (headless-verifiable).
  bool completionVisible() const { return completionView_ && completionView_->visible(); }
  size_t completionRowCount() const { return completionRows_.size(); }
  int completionSelected() const { return completionView_ ? completionView_->selectedIndex() : -1; }
  /// Sets the input text as if typed (fires the completion refresh).
  void setInputTextForTest(const std::string& text);

 private:
  void onSubmit();
  void appendLogLine(const std::string& line);
  void trimLogView();
  void syncLogView();
  void syncCompletionView();

  // ---- autocomplete (shares the registry-agnostic Completer unit) ----
  void refreshCompletion();
  void showCompletion();
  void hideCompletion();
  bool dropdownVisible() const { return completionVisible(); }
  void tabComplete();
  void acceptCompletion();
  void moveCompletionSelection(int delta);
  static std::string formatCompletionRow(const CompletionCandidate& c, size_t nameWidth);

  cConsoleReg& registry_;
  cRegCompletionSource completionSource_;  ///< injected candidate source (the Fragment seam)
  Completer completer_;
  lithogui::ILithoGui* gui_ = nullptr;
  // Owned only when the console panel became the GUI root; when the panel
  // joined an existing root tree, that tree owns it and panel_ is borrowed.
  std::unique_ptr<lithogui::Widget> ownedRoot_;
  lithogui::Widget* panel_ = nullptr;
  lithogui::ListBox* logView_ = nullptr;
  lithogui::ListBox* completionView_ = nullptr;
  lithogui::TextField* input_ = nullptr;
  std::vector<CompletionCandidate> completionRows_;
  std::vector<std::string> logLines_;  ///< the log view's model (capped at kMaxViewLines)
  std::string lastInputSeen_;          ///< input text at the last completion refresh

  bool open_ = false;
  std::vector<std::string> history_;
  int historyPos_ = -1;  ///< -1 = not browsing (at the fresh line)
  std::string pendingInput_;  ///< input stashed while browsing history
  uint64_t tapSeqSeen_ = 0;
  std::chrono::steady_clock::time_point lastToggle_{};

  static constexpr size_t kMaxHistory = 64;
  static constexpr size_t kMaxViewLines = 512;  ///< matches the log tap capacity
  static constexpr float kHeightFraction = 0.42f;
  static constexpr size_t kMaxCompletionRows = 24;    ///< dropdown lists at most this many rows
  static constexpr size_t kMaxCompletionVisible = 8;  ///< rows shown before the list scrolls
};

}  // namespace litholattice::console
