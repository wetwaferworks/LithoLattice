// lithovfs_mock.mjs ... run a LithoRoot test module under Node with
// lk_store_* backed by an in-memory map (the OPFS stand-in). Checks
// that every write went through lk_store by dumping the store.
import fs from 'fs';
const file = process.argv[2];
const mod = new WebAssembly.Module(fs.readFileSync(file));
const mem = new WebAssembly.Memory({initial: 4096, maximum: 9600, shared: true});
const E = {ENOENT: 44, EEXIST: 20, EBADF: 8, EINVAL: 28, ENOTEMPTY: 55, ENOTDIR: 54, EISDIR: 31};
const files = new Map();             // path -> {data: Uint8Array, mtime}
const dirs = new Set(['/']);
const pack = new Map([['pack/readme.txt', new TextEncoder().encode('from the pack')]]);
const packFds = new Map(); let nextPackFd = 10;
const handles = new Map(); let nextH = 1; const calls = {};
const str = (p, n) => Buffer.from(new Uint8Array(mem.buffer, p, n)).toString();
const parent = p => p.slice(0, p.lastIndexOf('/')) || '/';
const now = () => BigInt(Date.now()) * 1000000n;
const store = {
  lk_store_open(p, n, fl) { const k = str(p, n);
    if (dirs.has(k)) return -E.EISDIR;
    if (!files.has(k)) { if (!(fl & 4)) return -E.ENOENT; if (!dirs.has(parent(k))) return -E.ENOENT; files.set(k, {data: new Uint8Array(0), mtime: now()}); }
    else if ((fl & 4) && (fl & 16)) return -E.EEXIST;
    if (fl & 8) files.get(k).data = new Uint8Array(0);
    handles.set(nextH, k); return nextH++; },
  lk_store_read(h, buf, len, off) { const k = handles.get(h); if (!k) return -E.EBADF;
    const d = files.get(k).data; const o = Number(off); if (o >= d.length) return 0;
    const n = Math.min(len, d.length - o); new Uint8Array(mem.buffer, buf, n).set(d.subarray(o, o + n)); return n; },
  lk_store_write(h, buf, len, off) { const k = handles.get(h); if (!k) return -E.EBADF;
    const f = files.get(k); const o = Number(off); const end = o + len;
    if (end > f.data.length) { const nd = new Uint8Array(end); nd.set(f.data); f.data = nd; }
    f.data.set(new Uint8Array(mem.buffer, buf, len).slice(), o); f.mtime = now(); return len; },
  lk_store_truncate(h, size) { const k = handles.get(h); if (!k) return -E.EBADF;
    const f = files.get(k); const nd = new Uint8Array(Number(size)); nd.set(f.data.subarray(0, Number(size))); f.data = nd; return 0; },
  lk_store_flush(h) { return handles.has(h) ? 0 : -E.EBADF; },
  lk_store_close(h) { return handles.delete(h) ? 0 : -E.EBADF; },
  lk_store_stat(p, n, out) { const k = str(p, n); const dv = new DataView(mem.buffer, out, 24);
    if (dirs.has(k)) { dv.setUint32(0, 2, true); dv.setBigUint64(8, 0n, true); dv.setBigInt64(16, 0n, true); return 0; }
    const f = files.get(k); if (!f) return -E.ENOENT;
    dv.setUint32(0, 1, true); dv.setBigUint64(8, BigInt(f.data.length), true); dv.setBigInt64(16, f.mtime, true); return 0; },
  lk_store_mkdir(p, n) { const k = str(p, n); if (dirs.has(k) || files.has(k)) return -E.EEXIST; if (!dirs.has(parent(k))) return -E.ENOENT; dirs.add(k); return 0; },
  lk_store_list(p, n, buf, cap) { const k = str(p, n); if (!dirs.has(k)) return -E.ENOENT;
    const pre = k === '/' ? '/' : k + '/'; const out = [];
    for (const d of dirs) if (d !== k && d.startsWith(pre) && !d.slice(pre.length).includes('/')) out.push(d.slice(pre.length) + '/');
    for (const f of files.keys()) if (f.startsWith(pre) && !f.slice(pre.length).includes('/')) out.push(f.slice(pre.length));
    const b = Buffer.from(out.map(s => s + '\0').join('')); if (b.length <= cap) new Uint8Array(mem.buffer, buf, b.length).set(b); return b.length; },
  lk_store_remove(p, n) { const k = str(p, n);
    if (files.delete(k)) return 0;
    if (dirs.has(k)) { for (const x of [...dirs, ...files.keys()]) if (x.startsWith(k + '/')) return -E.ENOTEMPTY; dirs.delete(k); return 0; }
    return -E.ENOENT; },
  lk_store_rename(a, an, b, bn) { const s = str(a, an), d = str(b, bn);
    if (files.has(s)) { files.set(d, files.get(s)); files.delete(s); return 0; }
    if (!dirs.has(s)) return -E.ENOENT;
    for (const x of [...dirs]) if (x === s || x.startsWith(s + '/')) { dirs.delete(x); dirs.add(d + x.slice(s.length)); }
    for (const [x, v] of [...files]) if (x.startsWith(s + '/')) { files.delete(x); files.set(d + x.slice(s.length), v); }
    return 0; },
};
let out = '';
const imports = {};
for (const i of WebAssembly.Module.imports(mod)) {
  imports[i.module] ??= {};
  const set = f => imports[i.module][i.name] = f;
  if (i.kind === 'memory') set(mem);
  else if (store[i.name]) set((...a) => { calls[i.name] = (calls[i.name] || 0) + 1; return store[i.name](...a); });
  else if (i.name === 'lithosys_memory_grow') set((d, o) => { const old = mem.buffer.byteLength / 65536; if (old + d > 9600) return 48; mem.grow(d); new Uint32Array(mem.buffer, o, 1)[0] = old; return 0; });
  else if (i.name === 'lithosys_volume_open') set((p, o) => {   // one-file pack
    const b = new Uint8Array(mem.buffer); let e = p; while (b[e]) e++;
    const k = str(p, e - p).replace(/^\/+/, ''); if (!pack.has(k)) return 44;
    const fd = nextPackFd++; packFds.set(fd, {k, pos: 0}); new Uint32Array(mem.buffer, o, 1)[0] = fd; return 0; });
  else if (i.name === 'lithosys_fd_read') set((fd, iovs, n, o) => { const f = packFds.get(fd); if (!f) return 8;
    const dv = new DataView(mem.buffer); let got = 0; const d = pack.get(f.k);
    for (let j = 0; j < n; j++) { const bp = dv.getUint32(iovs + 8 * j, true), bl = dv.getUint32(iovs + 8 * j + 4, true);
      const m = Math.max(0, Math.min(bl, d.length - f.pos)); new Uint8Array(mem.buffer, bp, m).set(d.subarray(f.pos, f.pos + m)); f.pos += m; got += m; }
    dv.setUint32(o, got, true); return 0; });
  else if (i.name === 'lithosys_fd_seek') set((fd, off, wh, o) => { const f = packFds.get(fd); if (!f) return 8;
    const base = wh === 0 ? 0 : wh === 1 ? f.pos : pack.get(f.k).length; f.pos = base + Number(off);
    new BigUint64Array(mem.buffer, o, 1)[0] = BigInt(f.pos); return 0; });
  else if (i.name === 'lithosys_fd_close') set(fd => packFds.delete(fd) ? 0 : 8);
  else if (i.name === 'lithosys_fd_sync') set(() => 0);
  else if (i.name === 'lithosys_clock_now_ns') set((id, o) => { new BigUint64Array(mem.buffer, o, 1)[0] = process.hrtime.bigint(); return 0; });
  else if (i.name === 'lithosys_console_write') set((p, n) => { out += str(p, n); });
  else if (i.kind === 'function') set(() => { throw new Error('unexpected import call: ' + i.name); });
}
const inst = new WebAssembly.Instance(mod, imports);
inst.exports.__wasm_call_ctors();
const rc = inst.exports.__main_void();
console.log(JSON.stringify({rc, calls, files: [...files.keys()], dirs: [...dirs], open_handles: handles.size, console: out}));
process.exit(rc === 0 ? 0 : 1);
