// check_eh_catch.cpp ... toolchain piece, first-check (b), catcher
// translation unit. Returns a bitmask of what worked:
//   bit 0: plain throw caught with the right payload across TUs
//   bit 1: derived thrown, caught as base, kind() dispatches to 2
//          (proves the adjusted pointer + vtable survive the catch)
//   bit 2: rethrow from an inner catch is caught by the outer one
//   bit 3: catch(...) catches the plain error
//   bit 4: a local object's destructor ran during the unwind
//          (cleanup pads work, not just handler selection)
// Full pass = 31.

[[noreturn]] void lk_eh_throw_plain(int code);
[[noreturn]] void lk_eh_throw_derived();

struct LKProbeError {
  int code;
};

struct LKProbeBase {
  virtual ~LKProbeBase() {}
  virtual int kind() const { return 1; }
  int base_field;
};

namespace {
int g_guard_dtors = 0;
struct LKGuard {
  ~LKGuard() { ++g_guard_dtors; }
};
} // namespace

// The guard's frame sits between the thrower (another TU) and the
// catch: a correct unwind runs the destructor exactly once before
// the handler body executes.
extern "C" int lk_eh_cleanup_test() {
  g_guard_dtors = 0;
  try {
    LKGuard g;
    (void)g;
    lk_eh_throw_plain(5);
  } catch (const LKProbeError &) {
    return g_guard_dtors;
  }
  return -1;
}

extern "C" int lk_eh_probe() {
  int mask = 0;

  try {
    lk_eh_throw_plain(7);
  } catch (const LKProbeError &e) {
    if (e.code == 7) mask |= 1;
  }

  try {
    lk_eh_throw_derived();
  } catch (const LKProbeBase &b) {
    if (b.kind() == 2 && b.base_field == 11) mask |= 2;
  }

  try {
    try {
      lk_eh_throw_plain(9);
    } catch (const LKProbeError &) {
      throw;
    }
  } catch (const LKProbeError &e) {
    if (e.code == 9) mask |= 4;
  }

  try {
    lk_eh_throw_plain(3);
  } catch (...) {
    mask |= 8;
  }

  if (lk_eh_cleanup_test() == 1) mask |= 16;

  return mask;
}
