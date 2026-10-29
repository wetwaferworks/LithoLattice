#pragma once
// Headless test runs: route Debug CRT assert / error reports to stderr and
// suppress the abort message box and WER fault UI, so a failing assert fails
// the process instead of popping an Abort/Retry/Ignore dialog on the desktop
// (same policy as e6eb336 for litholattice_tests / engine_locomotion).
// No-op off Windows. Call first thing in a test program's main, or in the game
// exe only when a test mode was requested (interactive play keeps the dialog).
namespace litholattice {
void quietCrtDialogsForTests();
// True if any argv entry is a test-mode flag: "--selftest" or "--<name>-test".
bool argsRequestTestMode(int argc, char** argv);
}  // namespace litholattice