// How bright a picture from the TV's capture service is, over a part of it. The Control Center's
// glass over other apps blurs the TV's frames of the screen, and one that's black behind the panel
// is video the TV couldn't capture (protected, or its one video capture taken: PicCap's, for one).
// PNG, 8-bit RGB or RGBA, as the capture service writes it; Node's zlib does the rest.
'use strict';

const zlib = require('zlib');

const paeth = (a, b, c) => {
  const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
};

// the mean brightness (0-255, as luma) over x0..x1, y0..y1, given as fractions of the picture
function brightness(png, { x0 = 0, y0 = 0, x1 = 1, y1 = 1 } = {}) {
  if (png.length < 33 || png.readUInt32BE(0) !== 0x89504e47 || png.toString('latin1', 12, 16) !== 'IHDR') throw new Error('not a PNG');
  const w = png.readUInt32BE(16), h = png.readUInt32BE(20), bpp = { 2: 3, 6: 4 }[png[25]];
  if (png[24] !== 8 || !bpp || png[28]) throw new Error(`a PNG it doesn't read (depth ${png[24]}, colour type ${png[25]}, interlace ${png[28]})`);
  const idat = [];
  for (let at = 8; at + 8 <= png.length;) {
    const len = png.readUInt32BE(at), kind = png.toString('latin1', at + 4, at + 8);
    if (kind === 'IDAT') idat.push(png.subarray(at + 8, at + 8 + len));
    if (kind === 'IEND') break;
    at += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat)), stride = w * bpp;
  if (raw.length < h * (stride + 1)) throw new Error('a PNG cut short');
  const left = Math.floor(x0 * w), right = Math.ceil(x1 * w), top = Math.floor(y0 * h), bottom = Math.ceil(y1 * h);
  let sum = 0, n = 0;
  for (let y = 0; y < bottom; y++) { /* each row unfiltered in place, against the one above */
    const at = y * (stride + 1) + 1, up = at - stride - 1, filter = raw[at - 1];
    if (filter > 4) throw new Error(`a PNG row with filter ${filter}`);
    for (let i = 0; filter && i < stride; i++) {
      const a = i >= bpp ? raw[at + i - bpp] : 0, b = y ? raw[up + i] : 0, c = y && i >= bpp ? raw[up + i - bpp] : 0;
      raw[at + i] += filter === 1 ? a : filter === 2 ? b : filter === 3 ? (a + b) >> 1 : paeth(a, b, c);
    }
    if (y >= top) {
      for (let x = left; x < right; x++) {
        const p = at + x * bpp;
        sum += 0.299 * raw[p] + 0.587 * raw[p + 1] + 0.114 * raw[p + 2];
        n++;
      }
    }
  }
  return n ? sum / n : 0;
}

module.exports = { brightness };
