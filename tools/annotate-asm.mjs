// Replaces `call 0x<stub>` targets in an llvm-objdump listing with imported function names.
// usage: node annotate-asm.mjs <stubs.txt> < listing.asm
import { readFileSync } from "node:fs";
const stubs = new Map(readFileSync(process.argv[2], "utf8").trim().split("\n").map((l) => l.split(" ")));
const text = readFileSync(0, "utf8");
process.stdout.write(text.replace(/\b(call|jmp)\t(0x[0-9a-f]+)/g, (m, op, addr) => stubs.has(addr) ? `${op}\t${stubs.get(addr)}` : m));
