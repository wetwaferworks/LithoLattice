# Litho GUI integration into LithoLattice (2026-11-06)

JD's direction: integrate Litho GUI into the LithoLattice **engine**, replacing
the engine's current GUI; drop all the testing/demo stuff that was in place for
testing the GUI and pull out only the main interfaces. Cortana verifies on the
workstation; JD tests and debugs there.

## What landed

- **Core GUI** (from the standalone `~/workspace/litho-gui` tree, Build 27
  state, copied without modification except `#include` paths rewritten to
  engine style `litholattice/ui/...`, and one warning fix: the parameter
  of `TextField::moveVertical` was renamed `dir` -> `dy` in the header and
  the definition, because `dir` shadows the inherited `Widget::dir`
  member under the engine's `-Wshadow` module flags):
  - `src/ui/include/litholattice/ui/litho_gui.h` + `src/ui/litho_gui.cpp` —
    `ILithoGui` / `cLithoGui` (JD naming lock, lowercase c), the retained
    widget tree, `DrawList`, themes, `UiContext`.
  - `src/ui/include/litholattice/ui/litho_font.h` + `src/ui/litho_font.cpp` —
    the in-house font layer (Latin, Systerian numerals, Systerian Text,
    Systerian Break, Sumerian wedge faces; no font files, no libraries).
  - `src/ui/include/litholattice/ui/json.h` + `src/ui/json.cpp` — in-house
    JSON parser for themes.
  - `src/ui/include/litholattice/ui/markdown.h` + `src/ui/markdown.cpp` —
    markdown spans for `Label`.
  - Namespace stays `lithogui`.
- **Version**: Litho GUI carries its own subsystem version — versionName
  **0.1**, versionCode **27** (Build 27: universal Frame inheritance, 29
  widget classes) — recorded in `src/ui/CMakeLists.txt` and in a header
  note in `litho_gui.h`. It is separate from the engine build counter;
  the two are never conflated.
- **Theme**: `assets/ui/trace-default.json`.
- **Shaders**: `assets/shaders/ui/trace-border.glsl`,
  `assets/shaders/ui/panel-fill.glsl` (sources; compiling them stays a
  back-end job, as in the standalone base).
- **Selftest**: `src/ui/include/litholattice/ui/litho_gui_selftest.h` +
  `src/ui/litho_gui_selftest.cpp` — `litholattice::runLithoGuiSelftest()`,
  a compact integration check (tree build, layout at 1280x720, click
  dispatch, draw-list emission, theme parse), wired into
  `tests/engine_tests.cpp` as module `ui` in place of the old
  `ui::runUiSelfTest()`.

## What was removed (the old engine ui toolkit, replaced)

All old sources and headers under `src/ui`: `widget`, `painter`, `theme`,
`ui_root`, `controls`, `containers`, `text_buffer`, `text_edit`, `lists`,
`menus`, `dialog`, `widget_factory`, `mod_ui`, `ui_selftest` (`.cpp`s) and
`widget.h`, `painter.h`, `theme.h`, `ui_root.h`, `controls.h`,
`containers.h`, `text_buffer.h`, `text_edit.h`, `lists.h`, `menus.h`,
`dialog.h`, `widget_factory.h`, `mod_ui.h`, `ui_selftest.h`, `events.h`,
`geometry.h`, `ui_renderer.h`. `litholattice_ui` keeps its target name and
PUBLIC include dir and now builds only the new sources.

## What was dropped (not brought over)

The standalone GUI's testing/demo scaffolding stays in the standalone
`~/workspace/litho-gui` tree and the `ilithogui.git` hub, and did **not**
land here: `tests/`, `preview/`, `android/`, `attic/`, and the demo/test
screens `chat_screen.*`, `file_browser.*`, `inventory_screen.*` (mock-ups
built to test the GUI, not engine interfaces).

## Consumer changes

- **`src/render/hud_ui_renderer.{h,cpp}`** — `cHudUiRenderer` no longer
  implements the old `ui::IUiRenderer`. It is now a consumer of
  `lithogui::DrawList`: `render(const lithogui::DrawList&)` walks the
  commands in order and appends HudDraws to the HudLayer, reusing the
  established HudDraw/text/clip mechanics (whole-pixel snapping, clip
  stack, the fitted-icon pixel path, `downsampleIcon` /
  `invalidateHudIconCache`). The resolvers are now keyed by the GUI's
  integer texture ids (ImageQuads carry ids, not names).
  - PushClip/PopClip, FillRect, Line, Polyline (segment rects;
    axis-aligned exact, diagonal approximated by its bounding box —
    HUD limitation), TextRun (scale from font size over the 8x16 HUD
    glyph cell) and ImageQuad are handled.
  - **GlyphQuad is skipped**: the HUD has no glyph-atlas texture path,
    and glyph expansion only emits GlyphQuads when a glyph source is
    bound; the default engine path binds none, so text arrives as
    TextRuns.
  - **CustomDraw is skipped**: tag 1 is the shader Trace border, and the
    GUI emits a fallback Polyline for the same rect immediately before
    it in the stream, which is what the HUD draws.
  - It had no in-engine callers, so the API change is contained.
- **`src/console/debug_console.{h,cpp}`** — `cDebugConsole` ported to
  Litho GUI, preserving its behavior contract: tilde toggle with input
  confinement (never opens while another widget holds focus or a popup
  is open on the context; check runs before the 200 ms debounce), log
  view capped at 512 lines, panel height fraction 0.42, command history
  (64), HL1 completion dropdown (max 24 rows, 8 visible), and the test
  seams (`historySize`, `inputText`, `logLineCount`, `submitForTest`,
  `completionVisible/RowCount/Selected`, `setInputTextForTest`).
  New public API: `attach(lithogui::ILithoGui&)`,
  `handleTildeKey(lithogui::ILithoGui&, const lithogui::KeyEvent&)`,
  `handleKeyPreFilter(const lithogui::KeyEvent&)`, `update()`.
  The panel is a `lithogui::Panel` (Column) holding a `ListBox` log view
  over the console's own line store, the completion `ListBox`, and a
  `TextField` with `onSubmit`. Differences from the old port, honestly:
  - Litho GUI's `TextField` has no change callback, so live completion
    filtering polls the input text in `update()` (once per frame).
  - `lithogui::KeyCode` has no PageUp/PageDown, so pre-filter log
    paging by those keys is gone; the log ListBox wheel-scrolls.
  - Litho GUI's `Widget` has no child-reorder API, so `open()` no
    longer raises the panel above later root children; it draws in
    attach order.
  - The panel's fixed height applies the 0.42 fraction at a 720-line
    default viewport; sizing it from the live viewport each frame is
    host follow-on.
- **`src/console/console_selftest.cpp`** — console-language tests kept;
  the debug-console sections were adapted to the new API (a
  `lithogui::cLithoGui` replaces `ui::cUiRoot`, key events are
  `lithogui::KeyEvent`s, the confinement section focuses a real
  `lithogui::TextField`). Dropped from it: nothing tested the old ui
  toolkit directly except through the debug console, whose sections
  were ported rather than dropped; the only removals are the old
  `#include`s (`ui/text_edit.h`, `ui/ui_root.h`) and the old
  `UiEvent` helper factories.

## Honest follow-on (out of scope here)

The LithoKraft game repo still uses the old engine ui API and will not
compile against this engine until its GUI usage is migrated. That
migration is explicitly out of scope for this integration — it is the
Cortana/JD follow-on after this lands on the workstation.
