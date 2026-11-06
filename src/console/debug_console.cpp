// cDebugConsole implementation (Litho GUI port, 2026-11-06).
#include "litholattice/console/debug_console.h"

#include "litholattice/core/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace litholattice::console {
namespace {

const char* levelTag(LogLevel l) {
  switch (l) {
    case LogLevel::Info: return "";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR ";
  }
  return "";
}

std::string truncate(std::string s, size_t maxLen) {
  if (s.size() > maxLen) s = s.substr(0, maxLen - 3) + "...";
  return s;
}

}  // namespace

cDebugConsole::cDebugConsole(cConsoleReg& registry)
    : registry_(registry), completionSource_(registry), completer_(completionSource_) {
  lastToggle_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
}

void cDebugConsole::attach(lithogui::ILithoGui& gui) {
  gui_ = &gui;
  auto panel = std::make_unique<lithogui::Panel>("console");
  panel->dir = lithogui::LayoutDir::Column;
  // Top-anchored dropdown: top kHeightFraction of a 720-line default
  // viewport. (The host re-lays the GUI out at its real viewport each
  // frame; the panel's fixed height is the fraction applied at the
  // default — sizing the panel from the live viewport is host follow-on.)
  panel->fixedH = kHeightFraction * 720.0f;
  panel->padding = lithogui::Edges(4.0f);
  auto& stack = *panel;

  // Read-only log view: a ListBox over the console's own line store
  // (Litho GUI has no read-only text-editor widget; the lines are the
  // model, the list is the view, capped at kMaxViewLines by trimLogView).
  auto logView = std::make_unique<lithogui::ListBox>("console-log");
  logView->flex = 1.0f;
  logView_ = logView.get();
  stack.addChild(std::move(logView));

  // VS Code-style autocomplete dropdown: lives ABOVE the input, hidden
  // until there is something to show. A plain lithogui::ListBox
  // (composition, no new widgets); rows are "kind  name  detail" strings.
  auto completion = std::make_unique<lithogui::ListBox>("console-completion");
  completion->setVisible(false);
  completion->onSelect = [this](lithogui::ListBox&, int) { acceptCompletion(); };
  completionView_ = completion.get();
  stack.addChild(std::move(completion));

  auto input = std::make_unique<lithogui::TextField>("console-input");
  input->fixedH = 28.0f;
  input->onSubmit = [this](lithogui::TextField&, const std::string&) { onSubmit(); };
  input_ = input.get();
  stack.addChild(std::move(input));

  panel->setVisible(false);
  if (lithogui::Widget* root = gui.root()) {
    panel_ = root->addChild(std::move(panel));
  } else {
    panel_ = panel.get();
    ownedRoot_ = std::move(panel);
    gui.setRoot(ownedRoot_.get());
  }

  // Console output goes to the log view (and stays out of the engine log:
  // echoing it there would recurse through the tap).
  registry_.setOutput([this](const std::string& line) { appendLogLine(line); });
  // The engine's own log lines stream into the view via update().
  registry_.registerBuiltins(
#ifdef NDEBUG
      false
#else
      true
#endif
  );
  registry_.registerCommand("toggleconsole", "toggleconsole: opens/closes the debug console",
                            [this](cConsoleReg&, const ConsoleArgs&) { toggle(); });
  registry_.registerCommand("clear", "clear: clears the console log view",
                            [this](cConsoleReg&, const ConsoleArgs&) {
                              logLines_.clear();
                              syncLogView();
                            });
}

void cDebugConsole::open() {
  if (open_ || !gui_) return;
  open_ = true;
  // Note: the old toolkit raised the panel to the top of the root's
  // children on open; Litho GUI's Widget has no child-reorder API, so
  // the panel draws in attach order. (Raising is a host follow-on if
  // overlap ever shows it matters.)
  panel_->setVisible(true);
  gui_->context().setFocus(input_);
  historyPos_ = -1;
  pendingInput_.clear();
}

void cDebugConsole::close() {
  if (!open_ || !gui_) return;
  open_ = false;
  hideCompletion();
  panel_->setVisible(false);
  if (gui_->focused() == input_) gui_->context().setFocus(nullptr);
}

void cDebugConsole::toggle() {
  if (open_) close();
  else open();
}

bool cDebugConsole::handleTildeKey(lithogui::ILithoGui& gui, const lithogui::KeyEvent& e) {
  if (!e.down) return false;
  if (e.ch != U'~' && e.ch != U'`') return false;
  const auto now = std::chrono::steady_clock::now();
  const bool debounced = now - lastToggle_ < std::chrono::milliseconds(200);
  if (open_) {
    if (debounced) return true;  // key repeat: swallow
    lastToggle_ = now;
    close();
    return true;
  }
  // Closed: never steal the tilde from a focused text field / modal /
  // popup — this check comes BEFORE the debounce so typing "~" always
  // keeps working. Another widget holding focus, or any popup open on
  // the context, means the keyboard is spoken for.
  lithogui::Widget* focused = gui.focused();
  if (focused && focused != input_) return false;
  if (gui.context().popupCount() > 0) return false;
  if (debounced) return true;  // key repeat right after a toggle: swallow
  lastToggle_ = now;
  open();
  return true;
}

bool cDebugConsole::handleKeyPreFilter(const lithogui::KeyEvent& e) {
  if (!open_ || !e.down) return false;
  if (e.ctrl || e.alt) return false;
  // Tab: HL1-style completion (consumed before the TextField sees it).
  if (e.key == lithogui::KEY_TAB) {
    tabComplete();
    return true;
  }
  // While the autocomplete dropdown is visible it owns these keys.
  if (dropdownVisible()) {
    switch (e.key) {
      case lithogui::KEY_ESCAPE:
        hideCompletion();  // dismiss the dropdown; the console stays open
        return true;
      case lithogui::KEY_ENTER:
        acceptCompletion();
        return true;
      case lithogui::KEY_UP:
        moveCompletionSelection(-1);
        return true;
      case lithogui::KEY_DOWN:
        moveCompletionSelection(1);
        return true;
      default:
        break;
    }
  }
  switch (e.key) {
    case lithogui::KEY_ESCAPE:
      close();
      return true;
    case lithogui::KEY_UP:
    case lithogui::KEY_DOWN: {
      if (history_.empty()) return true;
      if (historyPos_ == -1) {
        pendingInput_ = input_->text();
        historyPos_ = int(history_.size()) - 1;
      } else if (e.key == lithogui::KEY_UP) {
        historyPos_ = std::max(0, historyPos_ - 1);
      } else {
        ++historyPos_;
        if (historyPos_ >= int(history_.size())) {
          historyPos_ = -1;  // back past the newest: restore the pending line
          input_->setText(pendingInput_);  // setText parks the caret at the end
          lastInputSeen_ = input_->text();
          return true;
        }
      }
      input_->setText(history_[size_t(historyPos_)]);  // caret to the end (setText)
      lastInputSeen_ = input_->text();
      return true;
    }
    default:
      // PageUp/PageDown log scrolling is not ported: lithogui::KeyCode
      // has no page keys. The log ListBox wheel-scrolls instead.
      return false;
  }
}

void cDebugConsole::update() {
  if (!gui_) return;
  const std::vector<LogRecord> records = engineLogTap().snapshot();
  bool added = false;
  for (const LogRecord& rec : records) {
    if (rec.seq <= tapSeqSeen_) continue;
    tapSeqSeen_ = rec.seq;
    appendLogLine("[" + rec.timestamp + "] " + levelTag(rec.level) + rec.text);
    added = true;
  }
  if (added) trimLogView();
  // The TextField has no change callback in Litho GUI, so the live
  // completion refresh polls the input text once per frame from here.
  if (open_ && input_ && input_->text() != lastInputSeen_) {
    lastInputSeen_ = input_->text();
    refreshCompletion();
  }
}

void cDebugConsole::onSubmit() {
  if (!input_) return;
  const std::string line = input_->text();
  hideCompletion();
  input_->setText("");
  lastInputSeen_.clear();
  historyPos_ = -1;
  pendingInput_.clear();
  if (line.empty()) return;
  if (history_.empty() || history_.back() != line) {
    history_.push_back(line);
    if (history_.size() > kMaxHistory) history_.erase(history_.begin());
  }
  appendLogLine("] " + line);  // Quake-style echo of the submitted line
  registry_.executeLine(line);
  trimLogView();
}

void cDebugConsole::appendLogLine(const std::string& line) {
  logLines_.push_back(line);
  syncLogView();
}

void cDebugConsole::trimLogView() {
  const int over = int(logLines_.size()) - int(kMaxViewLines);
  if (over > 0) logLines_.erase(logLines_.begin(), logLines_.begin() + over);
  syncLogView();
}

void cDebugConsole::syncLogView() {
  if (!logView_) return;
  logView_->items() = logLines_;
  // Keep the newest lines in view: the ListBox clamps the offset itself.
  logView_->setScrollY(1.0e9f);
}

void cDebugConsole::syncCompletionView() {
  if (!completionView_) return;
  size_t nameWidth = 0;
  for (const auto& c : completionRows_) nameWidth = std::max(nameWidth, c.name.size());
  std::vector<std::string> items;
  items.reserve(completionRows_.size());
  for (const auto& c : completionRows_) items.push_back(formatCompletionRow(c, nameWidth));
  completionView_->items() = std::move(items);
}

std::string cDebugConsole::inputText() const { return input_ ? input_->text() : std::string(); }

void cDebugConsole::setInputTextForTest(const std::string& text) {
  if (input_) {
    input_->setText(text);
    lastInputSeen_ = text;
    refreshCompletion();
  }
}

// ---- autocomplete ----

void cDebugConsole::refreshCompletion() {
  if (!open_ || !input_ || !completionView_) {
    hideCompletion();
    return;
  }
  completionRows_ = completer_.filter(input_->text());
  if (completionRows_.size() > kMaxCompletionRows) completionRows_.resize(kMaxCompletionRows);
  if (completionRows_.empty()) {
    hideCompletion();
    return;
  }
  showCompletion();
}

std::string cDebugConsole::formatCompletionRow(const CompletionCandidate& c, size_t nameWidth) {
  const char* tag = completionKindTag(c.kind);
  std::string row = tag + std::string(6 - std::strlen(tag), ' ') + c.name +
                    std::string(nameWidth + 2 - c.name.size(), ' ');
  if (c.kind == CompletionKind::Cvar && !c.detail.empty()) {
    row += "= " + c.detail;
    if (!c.description.empty()) row += "  " + truncate(c.description, 40);
  } else if (!c.description.empty()) {
    row += truncate(c.description, 48);
  }
  return row;
}

void cDebugConsole::showCompletion() {
  syncCompletionView();
  completionView_->setSelectedIndex(0);
  const float rowH = gui_ ? gui_->style().rowHeight : 22.0f;
  completionView_->fixedH =
      float(std::min(completionRows_.size(), kMaxCompletionVisible)) * rowH + 8.0f;
  completionView_->setVisible(true);
}

void cDebugConsole::hideCompletion() {
  completionRows_.clear();
  if (completionView_) {
    completionView_->setVisible(false);
    completionView_->setSelectedIndex(-1);
  }
}

void cDebugConsole::tabComplete() {
  if (!input_) return;
  const CompletionResult r = completer_.complete(input_->text());
  if (r.matches.empty()) return;  // no matches: no-op
  std::string line = r.completedLine;
  if (r.matches.size() == 1) line += " ";  // unique match: done, add the trailing space
  input_->setText(line);  // caret to the end (setText)
  lastInputSeen_ = input_->text();
  if (r.matches.size() > 1) {
    // HL1 lists the matches in the console.
    std::string names;
    for (const auto& m : r.matches) {
      if (!names.empty()) names += ", ";
      names += m.name;
    }
    appendLogLine(names);
    trimLogView();
  }
  refreshCompletion();
}

void cDebugConsole::acceptCompletion() {
  if (!dropdownVisible() || completionRows_.empty() || !input_) return;
  int sel = completionView_->selectedIndex();
  if (sel < 0 || size_t(sel) >= completionRows_.size()) sel = 0;
  const CompletionCandidate& c = completionRows_[size_t(sel)];
  const std::string line = input_->text();
  const CompletionSplit s = Completer::splitCompletionLine(line);
  std::string next;
  if (!s.head.empty() && s.hasSpace && Completer::isContextCommand(s.head)) {
    next = std::string(s.head) + " " + c.name;  // completing the `exec`/`map` argument
  } else if (!s.head.empty()) {
    const size_t headPos = size_t(s.head.data() - line.data());
    next = std::string(line.substr(0, headPos)) + c.name +
           std::string(line.substr(headPos + s.head.size()));
  } else {
    next = c.name;
  }
  input_->setText(next + " ");  // caret to the end (setText)
  lastInputSeen_ = input_->text();
  hideCompletion();
}

void cDebugConsole::moveCompletionSelection(int delta) {
  if (!dropdownVisible() || completionRows_.empty()) return;
  int sel = completionView_->selectedIndex();
  if (sel < 0) sel = 0;
  sel = std::clamp(sel + delta, 0, int(completionRows_.size()) - 1);
  completionView_->setSelectedIndex(sel);
}

size_t cDebugConsole::logLineCount() const { return logLines_.size(); }

void cDebugConsole::submitForTest(const std::string& line) {
  if (!input_) return;
  input_->setText(line);
  onSubmit();
}

}  // namespace litholattice::console
