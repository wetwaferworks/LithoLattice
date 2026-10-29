#include "litholattice/core/quiet_crt.h"

#include <cstring>

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>
#endif

namespace litholattice {

void quietCrtDialogsForTests() {
#if defined(_WIN32)
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
}

bool argsRequestTestMode(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!a || std::strncmp(a, "--", 2) != 0) continue;
    if (!std::strcmp(a, "--selftest")) return true;
    const size_t n = std::strlen(a);
    if (n > 7 && !std::strcmp(a + n - 5, "-test")) return true;
  }
  return false;
}

}  // namespace litholattice