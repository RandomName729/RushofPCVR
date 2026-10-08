// Summarises an Ngs2 call trace (SHADPS4_NGS2_TRACE, <user>/ngs2_trace.log).
//   node ngs2-trace.mjs <trace> params            control ids per rack type with sample payloads
//   node ngs2-trace.mjs <trace> racks             racks as created (deduplicated)
//   node ngs2-trace.mjs <trace> voice <name> [n]  the first n control lines of one voice
//   node ngs2-trace.mjs <trace> life [n]          lifecycles of n sampler voices, condensed
import { readFileSync } from "node:fs";

const [file, mode = "params", ...rest] = process.argv.slice(2);
const lines = readFileSync(file, "latin1").split(/\r?\n/);

if (mode === "params") {
  const stats = new Map();
  for (let i = 0; i < lines.length; i++) {
    const m = /^CTL R\d+\[(\w+)\]\.v(\d+) id=(\w+) size=(\d+) next=(-?\d+) data=(.*)$/.exec(lines[i]);
    if (!m) continue;
    const key = `${m[1]} ${m[3]}`;
    let entry = stats.get(key);
    if (!entry) stats.set(key, (entry = { count: 0, sizes: new Set(), samples: new Map(), extra: [] }));
    entry.count++;
    entry.sizes.add(m[4]);
    if (entry.samples.size < 6) entry.samples.set(m[6], (entry.samples.get(m[6]) ?? 0) + 1);
    else if (entry.samples.has(m[6])) entry.samples.set(m[6], entry.samples.get(m[6]) + 1);
    if (entry.extra.length < 4 && lines[i + 1]?.startsWith("    ")) {
      let j = i + 1;
      while (lines[j]?.startsWith("    ") && j < i + 8) entry.extra.push(lines[j++].trim());
    }
  }
  for (const [key, entry] of [...stats].sort()) {
    console.log(`${key}  x${entry.count}  size=${[...entry.sizes].join("/")}`);
    for (const [data, count] of entry.samples) console.log(`      ${count}x ${data}`);
    for (const extra of entry.extra.slice(0, 6)) console.log(`      | ${extra}`);
  }
} else if (mode === "racks") {
  const seen = new Map();
  for (let i = 0; i < lines.length; i++) {
    if (!lines[i].startsWith("RACK create")) continue;
    let text = lines[i].replace(/R\d+ sys=S\d+ /, "");
    let j = i + 1;
    while (lines[j]?.startsWith("    ")) text += "\n" + lines[j++];
    seen.set(text, (seen.get(text) ?? 0) + 1);
  }
  for (const [text, count] of seen) console.log(`x${count} ${text}`);
} else if (mode === "voice") {
  const name = rest[0];
  let left = Number(rest[1] ?? 80);
  for (let i = 0; i < lines.length && left > 0; i++) {
    if (lines[i].includes(name) && /^(CTL|STATE|FLAGS)/.test(lines[i])) {
      console.log(`${i + 1}: ${lines[i].slice(0, 230)}`);
      let j = i + 1;
      while (lines[j]?.startsWith("    ")) console.log(`      ${lines[j++].trim().slice(0, 220)}`);
      left--;
    }
  }
} else if (mode === "life") {
  // Order of control ids per sampler voice between setups.
  const want = Number(rest[0] ?? 12);
  const current = new Map();
  const shapes = new Map();
  const flush = (voice) => {
    const sequence = current.get(voice);
    if (!sequence) return;
    const key = sequence.join(" ");
    shapes.set(key, (shapes.get(key) ?? 0) + 1);
    current.delete(voice);
  };
  for (const line of lines) {
    const m = /^CTL (R\d+\[1000\]\.v\d+) id=(\w+) size=\d+ next=-?\d+ data=(\w*)/.exec(line);
    if (!m) continue;
    const id = m[2].replace(/^0+/, "");
    if (id === "10000000") flush(m[1]);
    const sequence = current.get(m[1]) ?? [];
    current.set(m[1], sequence);
    const tag = id === "6" ? `ev${parseInt(m[3].slice(0, 2), 16)}` : id.replace("100000", "s");
    if (sequence[sequence.length - 1]?.startsWith(tag + "*")) {
      const n = Number(sequence[sequence.length - 1].split("*")[1]) + 1;
      sequence[sequence.length - 1] = `${tag}*${n}`;
    } else if (sequence[sequence.length - 1] === tag) {
      sequence[sequence.length - 1] = `${tag}*2`;
    } else sequence.push(tag);
  }
  for (const voice of [...current.keys()]) flush(voice);
  const sorted = [...shapes].sort((a, b) => b[1] - a[1]);
  for (const [key, count] of sorted.slice(0, want)) console.log(`x${count}: ${key.replace(/\*\d+/g, "*")}`);
  console.log(`${shapes.size} distinct shapes`);
}
