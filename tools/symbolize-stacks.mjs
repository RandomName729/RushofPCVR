// Turns the "Native stacks" samples the core's watchdog writes to its log into something
// readable: per thread, where its samples were taken, with emulator addresses resolved to
// functions through the unstripped build.
//   node tools/symbolize-stacks.mjs [core.log] [shadps4 binary] [--depth n] [--thread name]
//                                    [--profile]
// --profile turns the samples of the chosen thread into two tables: how often each function was
// on the stack at all, and how often it was the innermost function of the emulator itself (so
// that time spent in the C library or the GPU driver counts for whoever called them).
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
let depth = 6;
let only = null;
let profile = false;
const positional = [];
while (args.length) {
  const arg = args.shift();
  if (arg === "--depth") depth = Number(args.shift());
  else if (arg === "--thread") only = args.shift();
  else if (arg === "--profile") profile = true;
  else positional.push(arg);
}
const log = positional[0] ?? join(root, "build/quest/run/core.log");
const binary = positional[1] ?? join(root, "build/arm64/shadps4");

// The pulled log repeats its tail, so the same sample can show up twice: keep one of each.
const samples = new Map();
let current = null;
for (const line of readFileSync(log, "latin1").split("\n")) {
  const header = line.match(/Native stacks, sample (\d+):/);
  if (header) {
    current = samples.has(header[1]) ? null : [];
    if (current) samples.set(header[1], current);
    continue;
  }
  const thread = line.match(/^  (.{16}) tid (\d+):(.*)$/);
  if (!thread) {
    if (!line.startsWith("  ")) current = null;
    continue;
  }
  if (current) current.push({ name: thread[1].trim(), tid: thread[2], frames: thread[3].trim() });
}

// Addresses inside the emulator are bare, those in libraries come in brackets.
const framePattern = /\[([^\]]*)\]|(0x[0-9a-f]+)/g;
const addresses = new Set();
for (const threads of samples.values()) {
  for (const thread of threads) {
    for (const match of thread.frames.matchAll(framePattern)) {
      if (match[2]) addresses.add(match[2]);
    }
  }
}
const names = new Map();
if (addresses.size) {
  const list = [...addresses];
  // Two lines per address: the function, then where it is.
  const output = execFileSync(
    join(root, "tools/llvm/bin/llvm-addr2line.exe"),
    ["-f", "-C", "-e", binary, ...list],
    { encoding: "utf8", maxBuffer: 1 << 26 },
  );
  const lines = output.split("\n").map((line) => line.trim());
  list.forEach((address, i) => {
    // Arguments and template arguments make names unreadably long.
    let fn = lines[2 * i] || "?";
    const paren = fn.indexOf("(");
    if (paren > 0) fn = fn.slice(0, paren);
    for (let previous; previous !== fn; ) {
      previous = fn;
      fn = fn.replace(/<[^<>]*>/g, "");
    }
    names.set(address, fn);
  });
}

const describe = (frames) => {
  const parts = [];
  for (const match of frames.matchAll(framePattern)) {
    parts.push(match[1] !== undefined ? `[${match[1]}]` : names.get(match[2]) ?? match[2]);
  }
  return parts.slice(0, depth).join(" < ");
};

if (profile) {
  const inclusive = new Map();
  const self = new Map();
  let total = 0;
  for (const threads of samples.values()) {
    for (const thread of threads) {
      if (only && !thread.name.includes(only)) continue;
      if (thread.frames === "did not answer") continue;
      ++total;
      const seen = new Set();
      let innermost = null;
      for (const match of thread.frames.matchAll(framePattern)) {
        let name;
        if (match[1] !== undefined) {
          // A library frame: the library is what matters, not the offset.
          const exported = match[1].split(" ")[1];
          name = `[${match[1].split("+")[0]}${exported ? " " + exported : ""}]`;
        } else {
          name = names.get(match[2]) ?? match[2];
          innermost ??= name;
        }
        seen.add(name);
      }
      for (const name of seen) inclusive.set(name, (inclusive.get(name) ?? 0) + 1);
      innermost ??= "(outside the emulator)";
      self.set(innermost, (self.get(innermost) ?? 0) + 1);
    }
  }
  const table = (title, map, limit) => {
    console.log(`
${title}`);
    for (const [name, count] of [...map].sort((a, b) => b[1] - a[1]).slice(0, limit)) {
      console.log(`  ${((100 * count) / total).toFixed(1).padStart(5)}%  ${name}`);
    }
  };
  console.log(`${total} samples of ${only ?? "all threads"}`);
  table("on the stack", inclusive, 70);
  table("innermost emulator function", self, 60);
  process.exit(0);
}

// Per thread: how many samples ended where.
const perThread = new Map();
for (const threads of samples.values()) {
  for (const thread of threads) {
    if (only && !thread.name.includes(only)) continue;
    const key = `${thread.name} (${thread.tid})`;
    const places = perThread.get(key) ?? new Map();
    const place = thread.frames === "did not answer" ? "(did not answer)" : describe(thread.frames);
    places.set(place, (places.get(place) ?? 0) + 1);
    perThread.set(key, places);
  }
}
console.log(`${samples.size} samples from ${log}`);
for (const [thread, places] of perThread) {
  console.log(`\n${thread}`);
  for (const [place, count] of [...places].sort((a, b) => b[1] - a[1])) {
    console.log(`  ${String(count).padStart(3)}x ${place}`);
  }
}
