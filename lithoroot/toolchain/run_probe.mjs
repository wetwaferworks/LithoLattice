// run_probe.mjs ... instantiate probe.wasm under a stub host that
// implements exactly the 14 frozen LithoSys calls, then run main.
// The thread spawn stub refuses with -LITHOSYS_EAGAIN, which is
// the point: the probe proves the fail-soft path end to end.
// Usage: node run_probe.mjs <probe.wasm>
import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) {
  console.error('usage: node run_probe.mjs <probe.wasm>');
  process.exit(2);
}

const memory = new WebAssembly.Memory({
  initial: 256, // 16 MiB, matching the link's --initial-memory
  maximum: 16384, // 1 GiB, matching the link's --max-memory
  shared: true,
});

const LITHOSYS = {
  OK: 0, EAGAIN: 6, EBADF: 8, EINVAL: 28, ENOENT: 44,
  ENOMEM: 48, ENOSYS: 52, ESPIPE: 70,
};

class LkExit extends Error {
  constructor(code) { super('lithosys_exit ' + code); this.code = code; }
}
class LkAbort extends Error {}

function u8() { return new Uint8Array(memory.buffer); }
function view() { return new DataView(memory.buffer); }

let exitCode = null;
const env = {
  memory,
  lithosys_memory_grow(deltaPages, outOldPages) {
    const old = memory.buffer.byteLength / 65536;
    try {
      memory.grow(deltaPages);
    } catch (e) {
      return LITHOSYS.ENOMEM;
    }
    if (outOldPages) view().setUint32(outOldPages, old, true);
    return LITHOSYS.OK;
  },
  lithosys_clock_now_ns(clockId, outNs) {
    const ns = clockId === 1
      ? BigInt(Date.now()) * 1000000n
      : BigInt(Math.floor(performance.now() * 1e6));
    view().setBigUint64(outNs, ns, true);
    return LITHOSYS.OK;
  },
  lithosys_thread_spawn(_startArg) {
    return -LITHOSYS.EAGAIN; // refuse: fail-soft is the exercise
  },
  lithosys_fd_write(fd, iovs, iovsLen, outWritten) {
    let total = 0;
    const dv = view();
    const bytes = u8();
    for (let i = 0; i < iovsLen; i++) {
      const base = iovs + i * 8;
      const ptr = dv.getUint32(base, true);
      const len = dv.getUint32(base + 4, true);
      if (fd === 1 || fd === 2) {
        process.stdout.write(new TextDecoder().decode(bytes.subarray(ptr, ptr + len)));
      }
      total += len;
    }
    if (outWritten) dv.setUint32(outWritten, total, true);
    return LITHOSYS.OK;
  },
  lithosys_fd_read(_fd, _iovs, _iovsLen, outRead) {
    if (outRead) view().setUint32(outRead, 0, true); // permanent EOF
    return LITHOSYS.OK;
  },
  lithosys_fd_close(_fd) { return LITHOSYS.OK; },
  lithosys_fd_seek(_fd, _offset, _whence, outPos) {
    if (outPos) view().setBigUint64(outPos, 0n, true);
    return LITHOSYS.OK;
  },
  lithosys_fd_sync(_fd) { return LITHOSYS.OK; },
  lithosys_volume_open(_path, _outFd) { return LITHOSYS.ENOSYS; },
  lithosys_console_write(buf, len) {
    process.stderr.write('[console_write] ' +
      new TextDecoder().decode(u8().subarray(buf, buf + len)));
  },
  lithosys_random_fill(buf, len) {
    const bytes = u8();
    for (let i = 0; i < len; i++) bytes[buf + i] = (i * 31 + 7) & 0xff;
    return LITHOSYS.OK;
  },
  lithosys_cpu_count() { return 4; },
  lithosys_exit(code) { exitCode = code; throw new LkExit(code); },
  lithosys_abort() { throw new LkAbort('lithosys_abort'); },
};

const bytes = readFileSync(path);
try {
  const { instance } = await WebAssembly.instantiate(bytes, { env });
  const ex = instance.exports;
  if (typeof ex.__wasm_init_tls === 'function') {
    ex.__wasm_init_tls(ex.lk_probe_tls_block());
    console.log('lk-probe: __wasm_init_tls done for the main thread');
  }
  if (typeof ex.__wasm_call_ctors === 'function') ex.__wasm_call_ctors();
  const rc = ex.main();
  console.log(`lk-probe: main returned ${rc}`);
  process.exit(rc === 0 ? 0 : 1);
} catch (e) {
  if (e instanceof LkExit) {
    console.log(`lk-probe: exited via lithosys_exit(${e.code})`);
    process.exit(e.code === 0 ? 0 : 1);
  }
  console.error('lk-probe: FAILED to run:', e);
  process.exit(3);
}
