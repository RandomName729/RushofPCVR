// Lists the NID imports of a PS4 ELF/SELF, resolved through shadPS4's aerolib table.
// usage: node list-imports.mjs <eboot.bin> [filter-regex]
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const aerolib = readFileSync(resolve(here, "../shadps4-arm64-main/scripts/aerolib.inl"), "latin1");
const names = new Map();
for (const m of aerolib.matchAll(/STUB\("([^"]{11})",\s*([A-Za-z0-9_]+)\)/g)) names.set(m[1], m[2]);

const bin = readFileSync(process.argv[2]).toString("latin1");
const filter = process.argv[3] ? new RegExp(process.argv[3]) : null;
const seen = new Map();
for (const m of bin.matchAll(/([A-Za-z0-9+\-]{11})#([A-Za-z0-9+\-]{1,3})#([A-Za-z0-9+\-]{1,3})/g)) {
  if (!seen.has(m[1])) seen.set(m[1], { lib: m[2], mod: m[3] });
}
const rows = [...seen].map(([nid, v]) => ({ nid, name: names.get(nid) ?? "?", ...v }));
rows.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
let shown = 0;
for (const r of rows) {
  if (filter && !filter.test(r.name)) continue;
  console.log(`${r.nid}  ${r.lib.padEnd(3)} ${r.mod.padEnd(3)} ${r.name}`);
  shown++;
}
console.error(`total imports: ${rows.length}, shown: ${shown}, unresolved: ${rows.filter((r) => r.name === "?").length}`);
