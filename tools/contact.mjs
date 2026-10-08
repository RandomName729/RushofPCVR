// Builds a contact sheet from emulator screenshots so a whole run can be reviewed at a glance.
// Only the left eye of each side-by-side frame is kept.
//   node contact.mjs <out.png> [--cols n] [--width px] [--full] [--gain x] <shot.png>...
// --full keeps both eyes, --gain brightens dark frames.
import { readFileSync, writeFileSync } from "node:fs";
import { deflateSync, inflateSync } from "node:zlib";

function decodePng(path) {
  const buf = readFileSync(path);
  let pos = 8;
  let width = 0, height = 0, colorType = 0, bitDepth = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString("latin1", pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
    } else if (type === "IDAT") {
      idat.push(data);
    }
    pos += 12 + len;
  }
  if (bitDepth !== 8 || (colorType !== 2 && colorType !== 6)) throw new Error("unsupported PNG");
  const bpp = colorType === 6 ? 4 : 3;
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * bpp;
  const pixels = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const out = pixels.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? pixels.subarray((y - 1) * stride, y * stride) : null;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? out[x - bpp] : 0;
      const b = prev ? prev[x] : 0;
      const c = prev && x >= bpp ? prev[x - bpp] : 0;
      let v = line[x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[x] = v & 0xff;
    }
  }
  return { width, height, bpp, pixels };
}

const crcTable = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(buf) {
  let c = 0xffffffff;
  for (const byte of buf) c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, "latin1");
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}
function encodePng(width, height, rgb) {
  const raw = Buffer.alloc(height * (width * 3 + 1));
  for (let y = 0; y < height; y++) {
    rgb.copy(raw, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw, { level: 6 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

const args = process.argv.slice(2);
const out = args.shift();
let cols = 6, cellWidth = 300, full = false, gain = 1;
const files = [];
while (args.length) {
  const arg = args.shift();
  if (arg === "--cols") cols = Number(args.shift());
  else if (arg === "--width") cellWidth = Number(args.shift());
  else if (arg === "--full") full = true;
  else if (arg === "--gain") gain = Number(args.shift());
  else files.push(arg);
}
if (!out || !files.length) {
  console.error("usage: contact.mjs <out.png> [--cols n] [--width px] [--full] <shot.png>...");
  process.exit(1);
}

const cells = [];
for (const file of files) {
  try {
    const img = decodePng(file);
    const srcWidth = full ? img.width : img.width >> 1;
    const cellHeight = Math.round((cellWidth * img.height) / srcWidth);
    const cell = Buffer.alloc(cellWidth * cellHeight * 3);
    // Box filter: average every source pixel that lands in a destination pixel.
    const sx = srcWidth / cellWidth, sy = img.height / cellHeight;
    for (let y = 0; y < cellHeight; y++) {
      const y0 = Math.floor(y * sy), y1 = Math.max(y0 + 1, Math.floor((y + 1) * sy));
      for (let x = 0; x < cellWidth; x++) {
        const x0 = Math.floor(x * sx), x1 = Math.max(x0 + 1, Math.floor((x + 1) * sx));
        let r = 0, g = 0, b = 0;
        for (let yy = y0; yy < y1; yy++) {
          for (let xx = x0; xx < x1; xx++) {
            const o = (yy * img.width + xx) * img.bpp;
            r += img.pixels[o];
            g += img.pixels[o + 1];
            b += img.pixels[o + 2];
          }
        }
        const n = (y1 - y0) * (x1 - x0);
        const o = (y * cellWidth + x) * 3;
        cell[o] = Math.min(255, (r / n) * gain);
        cell[o + 1] = Math.min(255, (g / n) * gain);
        cell[o + 2] = Math.min(255, (b / n) * gain);
      }
    }
    cells.push({ cell, cellHeight });
  } catch (error) {
    console.error(`skipped ${file}: ${error.message}`);
  }
}

const gap = 4;
const cellHeight = Math.max(...cells.map((c) => c.cellHeight));
const rows = Math.ceil(cells.length / cols);
const width = cols * cellWidth + (cols + 1) * gap;
const height = rows * cellHeight + (rows + 1) * gap;
const sheet = Buffer.alloc(width * height * 3, 0x40);
cells.forEach(({ cell, cellHeight: h }, index) => {
  const ox = gap + (index % cols) * (cellWidth + gap);
  const oy = gap + Math.floor(index / cols) * (cellHeight + gap);
  for (let y = 0; y < h; y++) {
    cell.copy(sheet, ((oy + y) * width + ox) * 3, y * cellWidth * 3, (y + 1) * cellWidth * 3);
  }
});
writeFileSync(out, encodePng(width, height, sheet));
console.log(`${out}: ${cells.length} frames, ${width}x${height}`);
