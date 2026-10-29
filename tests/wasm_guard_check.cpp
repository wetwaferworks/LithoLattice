/* wasm_guard_check.cpp ... review C1 regression, wasm32 side.
 *
 * Compiled (not run) for --target=wasm32 by
 * toolchain/run-native-suites.sh wasm_guard. The static_assert pins
 * LithoAbi's guard type to the 4-byte ARM-ABI guard WebAssembly
 * uses; the runner then checks the IR the compiler emits for the
 * real function-local static below is `internal global i32 0,
 * align 4`, i.e. that the type the runtime accesses and the object
 * the compiler allocates agree. Before the C1 fix the runtime used
 * `long long*` and 64-bit atomics on that 4-byte object.
 */
#include "cxxabi.h"

static_assert(sizeof(__cxxabiv1::__lithoabi_guard_t) == 4,
              "wasm32 guard must be 32 bits (ARM C++ ABI)");
static_assert(alignof(__cxxabiv1::__lithoabi_guard_t) == 4,
              "wasm32 guard alignment");

struct Probe {
  int v;
  Probe();
};

int wasm_guard_probe() {
  static Probe p; /* guarded, thread-safe statics on */
  return p.v;
}
