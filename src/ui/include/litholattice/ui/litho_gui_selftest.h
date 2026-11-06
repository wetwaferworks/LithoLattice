// Compact integration selftest for the Litho GUI subsystem inside the engine:
// builds a small cLithoGui tree, lays it out, dispatches a click, emits a frame
// and parses the Trace theme. (The GUI's full standalone test suites live with
// the standalone Litho GUI tree; they were testing scaffolding and did not land
// in the engine — see src/ui/LITHO-GUI-INTEGRATION.md.)
#pragma once

namespace litholattice {

/// Returns true when every integration check passes.
bool runLithoGuiSelftest();

}  // namespace litholattice
