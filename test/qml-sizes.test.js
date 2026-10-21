/* The Control Center over other apps (native/qml) is drawn as Home+'s own (native/src/main.c), and
 * its sizes are copies of the launcher's: these keep the two in step. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const C = fs.readFileSync(path.join(ROOT, 'native/src/main.c'), 'utf8');
const QML = fs.readFileSync(path.join(ROOT, 'native/qml/ControlCenterView.qml'), 'utf8');

/* a #define's number, in main.c */
function define(name) {
  const m = new RegExp(`^#define ${name} (\\d+)\\b`, 'm').exec(C);
  assert.ok(m, `main.c defines ${name}`);
  return Number(m[1]);
}
/* a readonly int's value, in ControlCenterView.qml, worked out from the ones before it */
function prop(name, seen = {}) {
  const m = new RegExp(`readonly property int ${name}: ([^\\n/]+)`).exec(QML);
  assert.ok(m, `ControlCenterView.qml has ${name}`);
  const names = Object.keys(seen);
  return Function(...names, `return (${m[1]});`)(...names.map((n) => seen[n]));
}

test('qml sizes: the overlay\'s grid is where Home+\'s is, in the same units', () => {
  const seen = {};
  for (const n of ['unit', 'gap', 'rightEdge', 'gridX', 'gridY', 'gridW']) seen[n] = prop(n, seen);
  assert.equal(seen.unit, define('CC_U'));
  assert.equal(seen.gap, define('CC_G'));
  assert.equal(seen.rightEdge, define('W') - define('DOCK_X'), 'RIGHT');
  assert.equal(seen.gridX, seen.rightEdge - 4 * define('CC_U') - 3 * define('CC_G'), 'cc_grid_x()');
  assert.match(C, /static float cc_grid_y\(void\) \{ return TOP_Y \+ BAR \+ 26; \}/, 'cc_grid_y() as the overlay has it');
  assert.equal(seen.gridY, define('TOP_Y') + define('BAR') + 26, 'cc_grid_y()');
});

test('qml sizes: the overlay\'s gallery is Home+\'s', () => {
  for (const [q, c] of [['galW', 'GAL_W'], ['galPad', 'GAL_PAD'], ['galHead', 'GAL_HEAD'], ['galGroup', 'GAL_GROUP'],
    ['galBtn', 'GAL_BTN'], ['galRow', 'GAL_ROW'], ['galNamePx', 'GAL_NAME_PX']]) {
    assert.equal(prop(q), define(c), `${q} is ${c}`);
  }
});
