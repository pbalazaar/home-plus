'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { brightness } = require('../service/png');
const { png } = require('../tools/native-preview/png');

/* a frame from the C3's capture service (DISPLAY, 160 x 90) while PicCap had its video capture: black */
const C3_BLACK = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAKAAAABaCAIAAACwpMoFAAAA1ElEQVR4Xu3QAQ0AAADCoPdPbQ8HESgMGDBgwIABAwYMGDBgwIABAwYM' +
  'GDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYM' +
  'GDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwIABAwYMGDBgwICBn4EBqRoAAc4hum0A' +
  'AAAASUVORK5CYII=', 'base64');

const luma = ([r, g, b]) => 0.299 * r + 0.587 * g + 0.114 * b;
const colour = (x, y) => [(x * 37) % 256, (y * 53) % 256, (x * y) % 256];
function mean(w, h, [x0, x1, y0, y1]) {
  let sum = 0;
  for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) sum += luma(colour(x, y));
  return sum / ((x1 - x0) * (y1 - y0));
}

test('png: the TV\'s black frame is black', () => {
  assert.equal(brightness(C3_BLACK), 0);
});

test('png: rows filtered every way PNG has are read back', () => {
  const want = mean(160, 90, [0, 160, 0, 90]);
  for (const f of [0, 1, 2, 3, 4]) assert.ok(Math.abs(brightness(png(160, 90, colour, () => f)) - want) < 1e-6, `filter ${f}`);
  assert.ok(Math.abs(brightness(png(160, 90, colour, (y) => y % 5)) - want) < 1e-6, 'a different one each row');
  const rgba = png(160, 90, (x, y) => colour(x, y).concat(255), (y) => y % 5);
  assert.ok(Math.abs(brightness(rgba) - want) < 1e-6, 'with alpha');
});

test('png: only the part asked for counts', () => {
  const part = brightness(png(160, 90, colour, (y) => y % 5), { x0: 0.75, x1: 0.98, y0: 0.04, y1: 0.93 });
  assert.ok(Math.abs(part - mean(160, 90, [120, 157, 3, 84])) < 1e-6);
  const half = png(160, 90, (x) => (x < 120 ? [255, 255, 255] : [0, 0, 0]));
  assert.equal(brightness(half, { x0: 0.75 }), 0, 'white beside it, black where it is');
});

test('png: what isn\'t a PNG it can read is refused', () => {
  assert.throws(() => brightness(Buffer.from('a frame of the video')), /not a PNG/);
  const grey16 = Buffer.from(C3_BLACK);
  grey16[24] = 16;
  assert.throws(() => brightness(grey16), /doesn't read/);
  assert.throws(() => brightness(C3_BLACK.subarray(0, 60)), /./, 'cut short');
});
