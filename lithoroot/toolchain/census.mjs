// census.mjs ... print the import census of a wasm module.
// Usage: node census.mjs <file.wasm>
import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) {
  console.error('usage: node census.mjs <file.wasm>');
  process.exit(2);
}
const bytes = readFileSync(path);
const mod = new WebAssembly.Module(bytes);
const imports = WebAssembly.Module.imports(mod);
console.log(`imports: ${imports.length}`);
for (const i of imports) {
  console.log(`  ${i.module}.${i.name} : ${i.kind}`);
}
const exportsList = WebAssembly.Module.exports(mod);
console.log(`exports: ${exportsList.length}`);
for (const e of exportsList) {
  console.log(`  ${e.name} : ${e.kind}`);
}
