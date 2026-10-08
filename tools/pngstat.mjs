// Prints brightness statistics for a PNG, overall and per cell of a grid, so screenshots can be
// checked without opening them.
//   node pngstat.mjs <file.png> [cols] [rows]
import { readFileSync } from "node:fs";
import { inflateSync } from "node:zlib";

export function decodePng(path) {
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

const [file, colsArg, rowsArg] = process.argv.slice(2);
if (file) {
  const { width, height, bpp, pixels } = decodePng(file);
  const cols = Number(colsArg ?? 8), rows = Number(rowsArg ?? 4);
  const sums = Array.from({ length: rows }, () => new Float64Array(cols));
  const counts = Array.from({ length: rows }, () => new Float64Array(cols));
  let nonBlack = 0, max = 0;
  for (let y = 0; y < height; y++) {
    const row = Math.min(rows - 1, Math.floor((y * rows) / height));
    for (let x = 0; x < width; x++) {
      const o = (y * width + x) * bpp;
      const lum = (pixels[o] + pixels[o + 1] + pixels[o + 2]) / 3;
      const col = Math.min(cols - 1, Math.floor((x * cols) / width));
      sums[row][col] += lum;
      counts[row][col]++;
      if (lum > 8) nonBlack++;
      if (lum > max) max = lum;
    }
  }
  console.log(`${width}x${height}, non-black ${(100 * nonBlack / (width * height)).toFixed(2)}%, max ${max.toFixed(0)}`);
  for (let r = 0; r < rows; r++) {
    console.log(Array.from(sums[r], (s, c) => (s / counts[r][c]).toFixed(0).padStart(4)).join(""));
  }
}
