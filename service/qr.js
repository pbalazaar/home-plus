// A QR code for a short text (a link), as a PNG: what the TV shows so a phone can open the keys page
// (keys.js). Byte mode, error correction M, versions 1-10 (up to 213 bytes), after Project Nayuki's
// QR Code generator (MIT). The PNG is greyscale; Node's zlib does the compression.
'use strict';

const zlib = require('zlib');

const ECC_PER_BLOCK = [0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26]; /* level M, by version */
const BLOCKS = [0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5];

function rawModules(ver) { /* the data and error correction modules: all but the patterns and format */
  let n = (16 * ver + 128) * ver + 64;
  if (ver >= 2) {
    const align = Math.floor(ver / 7) + 2;
    n -= (25 * align - 10) * align - 55;
    if (ver >= 7) n -= 36;
  }
  return n;
}
const dataCodewords = (ver) => Math.floor(rawModules(ver) / 8) - ECC_PER_BLOCK[ver] * BLOCKS[ver];

// Reed-Solomon over GF(2^8), modulo x^8 + x^4 + x^3 + x^2 + 1
function mul(x, y) {
  let z = 0;
  for (let i = 7; i >= 0; i--) {
    z = (z << 1) ^ ((z >>> 7) * 0x11d);
    z ^= ((y >>> i) & 1) * x;
  }
  return z;
}
function divisor(degree) {
  const r = new Array(degree).fill(0);
  r[degree - 1] = 1;
  let root = 1;
  for (let i = 0; i < degree; i++) {
    for (let j = 0; j < r.length; j++) {
      r[j] = mul(r[j], root);
      if (j + 1 < r.length) r[j] ^= r[j + 1];
    }
    root = mul(root, 0x02);
  }
  return r;
}
function remainder(data, div) {
  const r = new Array(div.length).fill(0);
  for (const b of data) {
    const factor = b ^ r.shift();
    r.push(0);
    div.forEach((c, i) => { r[i] ^= mul(c, factor); });
  }
  return r;
}

// the data codewords, split into blocks, each with its error correction, interleaved
function codewords(text) {
  const bytes = Buffer.from(String(text), 'utf8');
  let ver = 1;
  while (ver <= 10 && 4 + (ver < 10 ? 8 : 16) + bytes.length * 8 > dataCodewords(ver) * 8) ver++;
  if (ver > 10) throw new Error('too long for a QR code here');
  const bits = [];
  const put = (v, n) => { for (let i = n - 1; i >= 0; i--) bits.push((v >>> i) & 1); };
  put(4, 4); /* byte mode */
  put(bytes.length, ver < 10 ? 8 : 16);
  for (const b of bytes) put(b, 8);
  const cap = dataCodewords(ver) * 8;
  put(0, Math.min(4, cap - bits.length));
  put(0, (8 - (bits.length % 8)) % 8);
  for (let pad = 0xec; bits.length < cap; pad ^= 0xec ^ 0x11) put(pad, 8);
  const data = [];
  for (let i = 0; i < bits.length; i += 8) data.push(bits.slice(i, i + 8).reduce((a, b) => (a << 1) | b, 0));

  const nblocks = BLOCKS[ver], ecc = ECC_PER_BLOCK[ver], raw = Math.floor(rawModules(ver) / 8);
  const nshort = nblocks - (raw % nblocks), shortLen = Math.floor(raw / nblocks), div = divisor(ecc);
  const blocks = [];
  for (let i = 0, k = 0; i < nblocks; i++) {
    const dat = data.slice(k, k + shortLen - ecc + (i < nshort ? 0 : 1));
    k += dat.length;
    const e = remainder(dat, div);
    if (i < nshort) dat.push(0);
    blocks.push(dat.concat(e));
  }
  const out = [];
  for (let i = 0; i < blocks[0].length; i++) {
    blocks.forEach((b, j) => { if (i !== shortLen - ecc || j >= nshort) out.push(b[i]); });
  }
  return { ver, out };
}

const MASKS = [
  (x, y) => (x + y) % 2 === 0, (x, y) => y % 2 === 0, (x) => x % 3 === 0, (x, y) => (x + y) % 3 === 0,
  (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0, (x, y) => ((x * y) % 2) + ((x * y) % 3) === 0,
  (x, y) => (((x * y) % 2) + ((x * y) % 3)) % 2 === 0, (x, y) => (((x + y) % 2) + ((x * y) % 3)) % 2 === 0
];

// the modules (true: dark), as rows
function encode(text) {
  const { ver, out } = codewords(text);
  const size = ver * 4 + 17;
  const grid = Array.from({ length: size }, () => new Array(size).fill(false));
  const fixed = Array.from({ length: size }, () => new Array(size).fill(false));
  const set = (x, y, dark) => { grid[y][x] = dark; fixed[y][x] = true; };

  for (let i = 0; i < size; i++) { set(6, i, i % 2 === 0); set(i, 6, i % 2 === 0); } /* timing */
  for (const [cx, cy] of [[3, 3], [size - 4, 3], [3, size - 4]]) { /* finders, with their separators */
    for (let dy = -4; dy <= 4; dy++) {
      for (let dx = -4; dx <= 4; dx++) {
        const x = cx + dx, y = cy + dy, d = Math.max(Math.abs(dx), Math.abs(dy));
        if (x >= 0 && x < size && y >= 0 && y < size) set(x, y, d !== 2 && d !== 4);
      }
    }
  }
  if (ver > 1) { /* alignment patterns */
    const n = Math.floor(ver / 7) + 2, step = Math.ceil((ver * 4 + 4) / (n * 2 - 2)) * 2, pos = [6];
    for (let p = size - 7; pos.length < n; p -= step) pos.splice(1, 0, p);
    pos.forEach((x, i) => pos.forEach((y, j) => {
      if ((i === 0 && j === 0) || (i === 0 && j === n - 1) || (i === n - 1 && j === 0)) return;
      for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) set(x + dx, y + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
    }));
  }
  const format = (mask) => { /* level M (0), the mask, and their BCH bits */
    const d = mask;
    let r = d;
    for (let i = 0; i < 10; i++) r = (r << 1) ^ ((r >>> 9) * 0x537);
    const bits = ((d << 10) | r) ^ 0x5412, bit = (i) => ((bits >>> i) & 1) === 1;
    for (let i = 0; i <= 5; i++) set(8, i, bit(i));
    set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
    for (let i = 9; i < 15; i++) set(14 - i, 8, bit(i));
    for (let i = 0; i < 8; i++) set(size - 1 - i, 8, bit(i));
    for (let i = 8; i < 15; i++) set(8, size - 15 + i, bit(i));
    set(8, size - 8, true);
  };
  format(0);
  if (ver >= 7) {
    let r = ver;
    for (let i = 0; i < 12; i++) r = (r << 1) ^ ((r >>> 11) * 0x1f25);
    const bits = (ver << 12) | r;
    for (let i = 0; i < 18; i++) {
      const dark = ((bits >>> i) & 1) === 1, a = size - 11 + (i % 3), b = Math.floor(i / 3);
      set(a, b, dark); set(b, a, dark);
    }
  }

  let i = 0; /* the codewords, zigzagging up and down in two-module columns from the right */
  for (let right = size - 1; right >= 1; right -= 2) {
    if (right === 6) right = 5;
    for (let v = 0; v < size; v++) {
      for (let j = 0; j < 2; j++) {
        const x = right - j, y = ((right + 1) & 2) === 0 ? size - 1 - v : v;
        if (!fixed[y][x] && i < out.length * 8) { grid[y][x] = ((out[i >>> 3] >>> (7 - (i & 7))) & 1) === 1; i++; }
      }
    }
  }

  // the mask that leaves the fewest runs, blocks and imbalance (a simpler score than the standard's)
  let best = null;
  for (let m = 0; m < 8; m++) {
    const g = grid.map((row, y) => row.map((dark, x) => (!fixed[y][x] && MASKS[m](x, y) ? !dark : dark)));
    let score = 0, darkN = 0;
    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        if (g[y][x]) darkN++;
        if (x < size - 1 && y < size - 1 && g[y][x] === g[y][x + 1] && g[y][x] === g[y + 1][x] && g[y][x] === g[y + 1][x + 1]) score += 3;
      }
    }
    for (let a = 0; a < size; a++) {
      for (const line of [g[a], g.map((row) => row[a])]) {
        let run = 1;
        for (let k = 1; k <= size; k++) {
          if (k < size && line[k] === line[k - 1]) { run++; continue; }
          if (run >= 5) score += run - 2;
          run = 1;
        }
      }
    }
    score += Math.floor(Math.abs(darkN * 20 - size * size * 10) / (size * size)) * 10;
    if (!best || score < best.score) best = { score, g, m };
  }
  grid.forEach((row, y) => row.forEach((_, x) => { grid[y][x] = best.g[y][x]; }));
  format(best.m);
  return grid;
}

const CRC = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4), crc = Buffer.alloc(4), td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  len.writeUInt32BE(data.length);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

// the QR code as a PNG: dark modules on white, `scale` pixels each, with the standard's 4-module margin
function png(text, scale = 10) {
  const grid = encode(text), n = grid.length + 8, px = n * scale;
  const rows = [];
  for (let y = 0; y < px; y++) {
    const row = Buffer.alloc(px + 1, 255);
    row[0] = 0; /* no filter */
    const gy = Math.floor(y / scale) - 4;
    for (let x = 0; x < px; x++) {
      const gx = Math.floor(x / scale) - 4;
      if (gy >= 0 && gy < grid.length && gx >= 0 && gx < grid.length && grid[gy][gx]) row[x + 1] = 0;
    }
    rows.push(row);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(px, 0);
  ihdr.writeUInt32BE(px, 4);
  ihdr[8] = 8; /* 8-bit greyscale */
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(Buffer.concat(rows))), chunk('IEND', Buffer.alloc(0))]);
}

module.exports = { encode, png };
