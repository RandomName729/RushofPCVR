// Assembles a Debian arm64 sysroot on a machine without WSL, a package manager or symlink
// privileges, for cross-compiling the emulator core with clang.
//   node mk-sysroot.mjs <sysroot dir> [--suite trixie] [--cache dir] <package>...
// Packages are taken as listed (no dependency resolution). Symbolic links inside the packages
// become copies; /lib and friends become junctions onto /usr.
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  copyFileSync, existsSync, mkdirSync, readFileSync, statSync, symlinkSync, writeFileSync,
} from "node:fs";
import { dirname, join, posix, resolve } from "node:path";

const args = process.argv.slice(2);
const sysroot = resolve(args.shift() ?? "");
let suite = "trixie";
let cache = join(dirname(sysroot), "deb-cache");
const wanted = [];
while (args.length) {
  const arg = args.shift();
  if (arg === "--suite") suite = args.shift();
  else if (arg === "--cache") cache = args.shift();
  else wanted.push(arg);
}
if (!sysroot || !wanted.length) {
  console.error("usage: mk-sysroot.mjs <sysroot dir> [--suite trixie] [--cache dir] <package>...");
  process.exit(1);
}
mkdirSync(cache, { recursive: true });
mkdirSync(sysroot, { recursive: true });

const mirror = "https://deb.debian.org/debian";

function decompress(tool, data) {
  const result = spawnSync(tool, ["-dc"], { input: data, maxBuffer: 1 << 30 });
  if (result.status !== 0) throw new Error(`${tool} failed: ${result.stderr}`);
  return result.stdout;
}

async function download(url, file) {
  if (existsSync(file) && statSync(file).size > 0) return readFileSync(file);
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
  const data = Buffer.from(await response.arrayBuffer());
  writeFileSync(file, data);
  return data;
}

// Later indexes win, so point-release and security updates replace the base version.
const indexes = [
  [`${mirror}/dists/${suite}/main/binary-arm64/Packages.xz`, mirror],
  [`${mirror}/dists/${suite}-updates/main/binary-arm64/Packages.xz`, mirror],
];
const packages = new Map();
for (const [url, base] of indexes) {
  const file = join(cache, createHash("sha1").update(url).digest("hex").slice(0, 12) + "-Packages.xz");
  let text;
  try {
    text = decompress("xz", await download(url, file)).toString("utf8");
  } catch (error) {
    console.warn(`skipping ${url}: ${error.message}`);
    continue;
  }
  for (const stanza of text.split("\n\n")) {
    const name = /^Package: (.+)$/m.exec(stanza)?.[1];
    const filename = /^Filename: (.+)$/m.exec(stanza)?.[1];
    const sha256 = /^SHA256: (.+)$/m.exec(stanza)?.[1];
    const version = /^Version: (.+)$/m.exec(stanza)?.[1];
    if (name && filename) packages.set(name, { url: `${base}/${filename}`, sha256, version });
  }
}

function* arMembers(deb) {
  if (deb.toString("latin1", 0, 8) !== "!<arch>\n") throw new Error("not an ar archive");
  let pos = 8;
  while (pos + 60 <= deb.length) {
    const name = deb.toString("latin1", pos, pos + 16).trim().replace(/\/$/, "");
    const size = Number(deb.toString("latin1", pos + 48, pos + 58).trim());
    yield { name, data: deb.subarray(pos + 60, pos + 60 + size) };
    pos += 60 + size + (size & 1);
  }
}

function* tarEntries(tar) {
  let pos = 0;
  let longName = null, longLink = null;
  let pax = {};
  const text = (start, length) => {
    const slice = tar.subarray(start, start + length);
    const end = slice.indexOf(0);
    return slice.toString("utf8", 0, end < 0 ? length : end);
  };
  while (pos + 512 <= tar.length) {
    if (tar[pos] === 0) break;
    let name = text(pos, 100);
    const size = parseInt(text(pos + 124, 12).trim() || "0", 8);
    const type = String.fromCharCode(tar[pos + 156] || 0x30);
    let link = text(pos + 157, 100);
    const prefix = text(pos + 345, 155);
    if (prefix) name = `${prefix}/${name}`;
    const data = tar.subarray(pos + 512, pos + 512 + size);
    pos += 512 + Math.ceil(size / 512) * 512;
    if (type === "L") { longName = data.toString("utf8").replace(/\0+$/, ""); continue; }
    if (type === "K") { longLink = data.toString("utf8").replace(/\0+$/, ""); continue; }
    if (type === "x" || type === "g") {
      for (const record of data.toString("utf8").split("\n")) {
        const match = /^\d+ ([^=]+)=(.*)$/.exec(record);
        if (match) pax[match[1]] = match[2];
      }
      continue;
    }
    name = pax.path ?? longName ?? name;
    link = pax.linkpath ?? longLink ?? link;
    longName = longLink = null;
    pax = {};
    yield { name: name.replace(/^\.\//, "").replace(/\/$/, ""), type, link, data };
  }
}

// Merged /usr: everything top-level that Debian symlinks into /usr is stored there directly.
const merged = ["lib", "bin", "sbin", "lib64"];
const canonical = (path) => {
  const parts = path.split("/").filter(Boolean);
  if (merged.includes(parts[0])) parts.unshift("usr");
  return parts.join("/");
};

const links = [];
let fileCount = 0;
for (const name of wanted) {
  const pkg = packages.get(name);
  if (!pkg) throw new Error(`package ${name} not found in ${suite}`);
  const debFile = join(cache, posix.basename(pkg.url));
  const deb = await download(pkg.url, debFile);
  if (pkg.sha256 && createHash("sha256").update(deb).digest("hex") !== pkg.sha256) {
    throw new Error(`${name}: checksum mismatch`);
  }
  let tar = null;
  for (const member of arMembers(deb)) {
    if (member.name === "data.tar.xz") tar = decompress("xz", member.data);
    else if (member.name === "data.tar.zst") tar = decompress("zstd", member.data);
    else if (member.name === "data.tar.gz") tar = decompress("gzip", member.data);
    else if (member.name === "data.tar") tar = member.data;
  }
  if (!tar) throw new Error(`${name}: no data archive`);
  for (const entry of tarEntries(tar)) {
    if (!entry.name) continue;
    const path = canonical(entry.name);
    const target = join(sysroot, path);
    if (entry.type === "5") {
      mkdirSync(target, { recursive: true });
    } else if (entry.type === "0" || entry.type === "7") {
      mkdirSync(dirname(target), { recursive: true });
      writeFileSync(target, entry.data);
      fileCount++;
    } else if (entry.type === "2" || entry.type === "1") {
      // Hard links name their target from the archive root, symlinks from their own directory.
      const resolved = entry.type === "1" || entry.link.startsWith("/")
        ? canonical(entry.link)
        : canonical(posix.normalize(posix.join(posix.dirname(entry.name), entry.link)));
      if (!path.startsWith("usr/share/doc/")) links.push({ path, resolved });
    }
  }
  console.log(`${name} ${pkg.version}`);
}

// "include" is not a Debian link: clang finds GCC through /lib and then walks up with "..",
// which Windows resolves lexically instead of through the junction.
for (const dir of [...merged, "include"]) {
  const junction = join(sysroot, dir);
  if (!existsSync(junction) && existsSync(join(sysroot, "usr", dir))) {
    symlinkSync(join(sysroot, "usr", dir), junction, "junction");
  }
}

// Links may point at other links, so keep resolving until nothing changes.
let pending = links;
let linked = 0;
for (let pass = 0; pass < 8 && pending.length; pass++) {
  const next = [];
  for (const link of pending) {
    const source = join(sysroot, link.resolved);
    const target = join(sysroot, link.path);
    if (!existsSync(source)) {
      next.push(link);
      continue;
    }
    if (statSync(source).isDirectory()) {
      if (!existsSync(target)) {
        mkdirSync(dirname(target), { recursive: true });
        symlinkSync(source, target, "junction");
      }
    } else {
      mkdirSync(dirname(target), { recursive: true });
      copyFileSync(source, target);
    }
    linked++;
  }
  pending = next;
}
console.log(`${fileCount} files, ${linked} links materialised, ${pending.length} dangling links skipped`);
for (const link of pending.slice(0, 40)) console.log(`  dangling: ${link.path} -> ${link.resolved}`);
