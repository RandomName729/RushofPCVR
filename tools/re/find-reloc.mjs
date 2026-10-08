// Finds where the game's data refers to a code address: virtual tables and other function
// pointers are filled in by relative relocations, so they do not show up in the disassembly.
//   node tools/re/find-reloc.mjs <elf> <address> [slots before] [slots after]
// Prints each slot holding the address, with the neighbouring slots (the rest of the table).
import { readFileSync } from "node:fs";

const [file, target, before = "8", after = "12"] = process.argv.slice(2);
const elf = readFileSync(file);
const wanted = BigInt(target);

const phoff = Number(elf.readBigUInt64LE(0x20));
const phentsize = elf.readUInt16LE(0x36);
const phnum = elf.readUInt16LE(0x38);
const segments = [];
for (let i = 0; i < phnum; ++i) {
  const at = phoff + i * phentsize;
  segments.push({
    type: elf.readUInt32LE(at),
    offset: Number(elf.readBigUInt64LE(at + 8)),
    vaddr: elf.readBigUInt64LE(at + 16),
    filesz: Number(elf.readBigUInt64LE(at + 32)),
  });
}
const dynamic = segments.find((s) => s.type === 2);
const dynlib = segments.find((s) => s.type === 0x61000000);
if (!dynamic || !dynlib) {
  throw new Error("not a PS4 executable: no dynamic or SCE dynlib data segment");
}

const tags = new Map();
for (let at = dynamic.offset; at < dynamic.offset + dynamic.filesz; at += 16) {
  tags.set(Number(elf.readBigInt64LE(at)), Number(elf.readBigUInt64LE(at + 8)));
}
const DT_SCE_RELA = 0x6100002f;
const DT_SCE_RELASZ = 0x61000031;
const rela = dynlib.offset + tags.get(DT_SCE_RELA);
const count = tags.get(DT_SCE_RELASZ) / 24;

const slots = new Map();
for (let i = 0; i < count; ++i) {
  const at = rela + i * 24;
  if (elf.readUInt32LE(at + 8) !== 8) {
    continue; // only R_X86_64_RELATIVE carries a plain address
  }
  slots.set(elf.readBigUInt64LE(at), elf.readBigInt64LE(at + 16));
}

const hex = (value) => "0x" + value.toString(16);
let found = 0;
for (const [slot, addend] of slots) {
  if (addend !== wanted) {
    continue;
  }
  ++found;
  console.log(`slot ${hex(slot)}:`);
  for (let k = -Number(before); k <= Number(after); ++k) {
    const neighbour = slot + BigInt(k * 8);
    const value = slots.get(neighbour);
    console.log(
      `  ${k === 0 ? ">" : " "} ${hex(neighbour)}  ${value === undefined ? "-" : hex(value)}`,
    );
  }
}
console.log(`${found} reference(s) to ${hex(wanted)} among ${slots.size} relative relocations`);
