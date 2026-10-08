// Condenses an emulator log: one line per distinct message (numbers folded), in order of first
// appearance, with how often it occurred.
//   node logsum.mjs <log> [--from <regex>] [--skip <regex>] [--only <regex>] [--max <n>]
import { readFileSync } from "node:fs";

const args = process.argv.slice(2);
const file = args.shift();
const opt = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
};
const from = opt("--from") ? new RegExp(opt("--from")) : null;
const only = opt("--only") ? new RegExp(opt("--only")) : null;
const skip = new RegExp(
  opt("--skip") ??
    "^\\[Core\\.Linker\\] <(Info|Warning)>|^\\[Config\\]|BACHATA_|FRAME_SLOT|GET_RENDER_FRAME|Skipped \\d+ duplicate",
);
const max = Number(opt("--max") ?? 200);

const seen = new Map();
let started = !from;
for (const raw of readFileSync(file, "latin1").split(/\r?\n/)) {
  if (!started) {
    if (!from.test(raw)) continue;
    started = true;
  }
  if (!raw || skip.test(raw) || (only && !only.test(raw))) continue;
  const line = raw.replace(/^\[([^\]]+)\] <(\w+)> \(([^)]*)\) /, "[$1|$2|$3] ");
  const key = line.replace(/0x[0-9a-fA-F]+/g, "0x#").replace(/\d+(\.\d+)?/g, "#");
  const entry = seen.get(key);
  if (entry) entry.count++;
  else seen.set(key, { line, count: 1 });
}
let shown = 0;
for (const { line, count } of seen.values()) {
  if (shown++ >= max) {
    console.log(`... ${seen.size - max} more distinct lines`);
    break;
  }
  console.log(`${String(count).padStart(6)}x ${line.slice(0, 250)}`);
}
