// Minimal PS4 SELF/ELF inspector used to reverse-engineer how a game calls HLE functions.
//
//   node ps4elf.mjs unwrap  <eboot.bin> <out.elf>      write a plain ELF (fSELF container removed)
//   node ps4elf.mjs imports <eboot.bin> [regex]        imported symbols with their PLT stub address
//   node ps4elf.mjs callers <eboot.bin> <regex>        call sites (E8/E9 rel32) of matching imports
//   node ps4elf.mjs bytes   <eboot.bin> <vaddr> <len>  hex dump of mapped bytes at a virtual address
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const SELF_MAGIC = 0x1d3d154f;
const PT_LOAD = 1, PT_DYNAMIC = 2, PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_RELRO = 0x61000010;
const DT = {
  SCE_JMPREL: 0x61000029, SCE_PLTRELSZ: 0x6100002d, SCE_RELA: 0x6100002f, SCE_RELASZ: 0x61000031,
  SCE_STRTAB: 0x61000035, SCE_STRSZ: 0x61000037, SCE_SYMTAB: 0x61000039, SCE_SYMTABSZ: 0x6100003f,
};

export function loadElf(path) {
  const file = readFileSync(path);
  let elfOff = 0;
  let selfSegments = null;
  if (file.readUInt32LE(0) === SELF_MAGIC) {
    const segCount = file.readUInt16LE(0x18);
    selfSegments = [];
    for (let i = 0; i < segCount; i++) {
      const o = 0x20 + i * 0x20;
      selfSegments.push({
        flags: file.readBigUInt64LE(o),
        offset: Number(file.readBigUInt64LE(o + 8)),
        fileSize: Number(file.readBigUInt64LE(o + 16)),
        memSize: Number(file.readBigUInt64LE(o + 24)),
      });
    }
    elfOff = 0x20 + segCount * 0x20;
  }
  const eh = file.subarray(elfOff);
  if (eh.readUInt32BE(0) !== 0x7f454c46) throw new Error("not an ELF");
  const phoff = Number(eh.readBigUInt64LE(0x20));
  const phentsize = eh.readUInt16LE(0x36);
  const phnum = eh.readUInt16LE(0x38);
  const phdrs = [];
  for (let i = 0; i < phnum; i++) {
    const o = phoff + i * phentsize;
    phdrs.push({
      index: i,
      type: eh.readUInt32LE(o),
      flags: eh.readUInt32LE(o + 4),
      offset: Number(eh.readBigUInt64LE(o + 8)),
      vaddr: Number(eh.readBigUInt64LE(o + 16)),
      filesz: Number(eh.readBigUInt64LE(o + 32)),
      memsz: Number(eh.readBigUInt64LE(o + 40)),
    });
  }
  // Where does each program header's data live in the container?
  const dataOf = (ph) => {
    if (!selfSegments) return file.subarray(ph.offset, ph.offset + ph.filesz);
    for (const seg of selfSegments) {
      const blocked = (seg.flags & 0x800n) !== 0n;
      const id = Number((seg.flags >> 20n) & 0xfffn);
      if (blocked && id === ph.index) return file.subarray(seg.offset, seg.offset + ph.filesz);
    }
    return null;
  };
  for (const ph of phdrs) ph.data = ph.filesz ? dataOf(ph) : null;
  // Headers such as PT_DYNAMIC and PT_TLS have no container segment of their own; their bytes
  // sit inside another header's file range.
  for (const ph of phdrs) {
    if (ph.data || !ph.filesz) continue;
    const host = phdrs.find((p) => p.data && p !== ph && ph.offset >= p.offset &&
                                   ph.offset + ph.filesz <= p.offset + p.filesz);
    if (host) ph.data = host.data.subarray(ph.offset - host.offset, ph.offset - host.offset + ph.filesz);
  }
  const headerLen = phoff + phnum * phentsize;
  return { file, elfOff, header: eh.subarray(0, headerLen), phdrs, isSelf: !!selfSegments };
}

export function unwrap(elf) {
  let size = elf.header.length;
  for (const ph of elf.phdrs) if (ph.data) size = Math.max(size, ph.offset + ph.filesz);
  const out = Buffer.alloc(size);
  elf.header.copy(out, 0);
  for (const ph of elf.phdrs) if (ph.data) ph.data.copy(out, ph.offset);
  // Present it as a normal shared object so stock binutils-style tools accept it.
  out.writeUInt16LE(3, 0x10); // e_type = ET_DYN
  out.writeUInt8(0, 7); // EI_OSABI = SYSV (was FreeBSD)
  out.writeBigUInt64LE(0n, 0x28); // e_shoff
  out.writeUInt16LE(0, 0x3c); // e_shnum
  out.writeUInt16LE(0, 0x3e); // e_shstrndx
  return out;
}

export function readVaddr(elf, vaddr, len) {
  for (const ph of elf.phdrs) {
    if ((ph.type === PT_LOAD || ph.type === PT_SCE_RELRO) && ph.data && vaddr >= ph.vaddr &&
        vaddr + len <= ph.vaddr + ph.filesz) {
      return ph.data.subarray(vaddr - ph.vaddr, vaddr - ph.vaddr + len);
    }
  }
  return null;
}

function nidNames() {
  const text = readFileSync(resolve(here, "../shadps4-arm64-main/scripts/aerolib.inl"), "latin1");
  const map = new Map();
  for (const m of text.matchAll(/STUB\("([^"]{11})",\s*([A-Za-z0-9_]+)\)/g)) map.set(m[1], m[2]);
  return map;
}

export function imports(elf) {
  const dynlib = elf.phdrs.find((p) => p.type === PT_SCE_DYNLIBDATA);
  const dynamic = elf.phdrs.find((p) => p.type === PT_DYNAMIC);
  if (!dynlib || !dynamic) throw new Error("no SCE dynamic data");
  const tags = new Map();
  for (let o = 0; o + 16 <= dynamic.data.length; o += 16) {
    const tag = Number(dynamic.data.readBigUInt64LE(o));
    if (tag === 0) break;
    if (!tags.has(tag)) tags.set(tag, Number(dynamic.data.readBigUInt64LE(o + 8)));
  }
  const d = dynlib.data;
  const strtab = tags.get(DT.SCE_STRTAB), symtab = tags.get(DT.SCE_SYMTAB);
  const names = nidNames();
  const symName = (index) => {
    const o = symtab + index * 24;
    const nameOff = strtab + d.readUInt32LE(o);
    let end = nameOff;
    while (d[end] !== 0) end++;
    return d.toString("latin1", nameOff, end);
  };
  const result = [];
  const readRela = (off, size, kind) => {
    for (let o = off; o + 24 <= off + size; o += 24) {
      const slot = Number(d.readBigUInt64LE(o));
      const info = d.readBigUInt64LE(o + 8);
      const type = Number(info & 0xffffffffn);
      const sym = Number(info >> 32n);
      if (type !== 7 && type !== 6) continue; // JUMP_SLOT / GLOB_DAT
      const raw = symName(sym);
      const nid = raw.split("#")[0];
      result.push({ slot, nid, raw, name: names.get(nid) ?? raw, kind: type === 7 ? "func" : "data" });
    }
  };
  if (tags.has(DT.SCE_JMPREL)) readRela(tags.get(DT.SCE_JMPREL), tags.get(DT.SCE_PLTRELSZ), "plt");
  if (tags.has(DT.SCE_RELA)) readRela(tags.get(DT.SCE_RELA), tags.get(DT.SCE_RELASZ), "rela");

  // Locate each import's PLT stub: `jmp qword ptr [rip + disp32]` pointing at its GOT slot.
  const text = elf.phdrs.find((p) => p.type === PT_LOAD && (p.flags & 1));
  const bySlot = new Map(result.map((r) => [r.slot, r]));
  const t = text.data;
  for (let i = 0; i + 6 <= t.length; i++) {
    if (t[i] !== 0xff || t[i + 1] !== 0x25) continue;
    const target = text.vaddr + i + 6 + t.readInt32LE(i + 2);
    const imp = bySlot.get(target);
    if (imp && imp.stub === undefined) imp.stub = text.vaddr + i;
  }
  return { list: result, text };
}

export function callers(elf, regex) {
  const { list, text } = imports(elf);
  const wanted = new Map();
  for (const imp of list) if (imp.stub !== undefined && regex.test(imp.name)) wanted.set(imp.stub, imp);
  const t = text.data;
  const hits = [];
  for (let i = 0; i + 5 <= t.length; i++) {
    if (t[i] !== 0xe8 && t[i] !== 0xe9) continue;
    const target = text.vaddr + i + 5 + t.readInt32LE(i + 1);
    const imp = wanted.get(target);
    if (imp) hits.push({ site: text.vaddr + i, kind: t[i] === 0xe8 ? "call" : "jmp", name: imp.name });
  }
  return hits;
}

const hex = (n) => "0x" + n.toString(16);
const [cmd, path, a, b] = process.argv.slice(2);
if (cmd === "unwrap") {
  const elf = loadElf(path);
  writeFileSync(a, unwrap(elf));
  for (const ph of elf.phdrs) {
    console.log(`phdr ${ph.index} type=${hex(ph.type)} flags=${ph.flags} vaddr=${hex(ph.vaddr)} filesz=${hex(ph.filesz)} memsz=${hex(ph.memsz)} off=${hex(ph.offset)}`);
  }
} else if (cmd === "imports") {
  const re = a ? new RegExp(a) : /./;
  const { list } = imports(loadElf(path));
  for (const i of list.filter((i) => re.test(i.name)).sort((x, y) => (x.name < y.name ? -1 : 1))) {
    console.log(`${i.kind} got=${hex(i.slot)} stub=${i.stub === undefined ? "-" : hex(i.stub)} ${i.name}`);
  }
} else if (cmd === "callers") {
  for (const h of callers(loadElf(path), new RegExp(a))) console.log(`${hex(h.site)} ${h.kind} ${h.name}`);
} else if (cmd === "bytes") {
  const buf = readVaddr(loadElf(path), Number(a), Number(b));
  console.log(buf ? buf.toString("hex") : "unmapped");
} else if (cmd) {
  console.error("unknown command");
  process.exit(2);
}
