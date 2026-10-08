// Prints what the relative relocations put into a range of the game's data: the entries of a
// virtual table, for one.
//   node tools/re/dump-slots.mjs <elf> <first address> <count of 8-byte slots>
import { readFileSync } from "node:fs";
const [file, first, n = "16"] = process.argv.slice(2);
const elf = readFileSync(file);
const phoff = Number(elf.readBigUInt64LE(0x20));
const phentsize = elf.readUInt16LE(0x36);
const phnum = elf.readUInt16LE(0x38);
const segments = [];
for (let i = 0; i < phnum; ++i) {
  const at = phoff + i * phentsize;
  segments.push({
    type: elf.readUInt32LE(at),
    offset: Number(elf.readBigUInt64LE(at + 8)),
    filesz: Number(elf.readBigUInt64LE(at + 32)),
  });
}
const dynamic = segments.find((s) => s.type === 2);
const dynlib = segments.find((s) => s.type === 0x61000000);
const tags = new Map();
for (let at = dynamic.offset; at < dynamic.offset + dynamic.filesz; at += 16) {
  tags.set(Number(elf.readBigInt64LE(at)), Number(elf.readBigUInt64LE(at + 8)));
}
const rela = dynlib.offset + tags.get(0x6100002f);
const count = tags.get(0x61000031) / 24;
const slots = new Map();
for (let i = 0; i < count; ++i) {
  const at = rela + i * 24;
  if (elf.readUInt32LE(at + 8) === 8) slots.set(elf.readBigUInt64LE(at), elf.readBigInt64LE(at + 16));
}
for (let k = 0; k < Number(n); ++k) {
  const slot = BigInt(first) + BigInt(k * 8);
  const value = slots.get(slot);
  console.log(`0x${slot.toString(16)} (+0x${(k * 8).toString(16)})  ${value === undefined ? "-" : "0x" + value.toString(16)}`);
}
