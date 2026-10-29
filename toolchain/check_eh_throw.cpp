// check_eh_throw.cpp ... toolchain piece, first-check (b), thrower
// translation unit. The catcher lives in check_eh_catch.cpp so the
// throw provably crosses a TU boundary. No standard library headers
// are used: the point is the compiler's own EH emission (tag,
// landing pads, the runtime calls the pads make), which LithoAbi
// must satisfy.

struct LKProbeError {
  int code;
};

struct LKProbeBase {
  virtual ~LKProbeBase() {}
  virtual int kind() const { return 1; }
  int base_field;
};

struct LKProbeDerived : LKProbeBase {
  int kind() const override { return 2; }
  int derived_field;
};

[[noreturn]] void lk_eh_throw_plain(int code) {
  throw LKProbeError{code};
}

[[noreturn]] void lk_eh_throw_derived() {
  LKProbeDerived d;
  d.base_field = 11;
  d.derived_field = 22;
  throw d;
}
