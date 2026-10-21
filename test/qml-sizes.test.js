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
/* a #define's colour, in main.c: "255, 90, 80" */
function defineRgb(name) {
  const m = new RegExp(`^#define ${name} (\\d+), (\\d+), (\\d+)\\b`, 'm').exec(C);
  assert.ok(m, `main.c defines ${name} as r, g, b`);
  return m.slice(1).map(Number);
}
/* a readonly int's value, in ControlCenterView.qml, worked out from the ones before it */
function prop(name, seen = {}) {
  const m = new RegExp(`readonly property int ${name}: ([^\\n/]+)`).exec(QML);
  assert.ok(m, `ControlCenterView.qml has ${name}`);
  const names = Object.keys(seen);
  return Function(...names, `return (${m[1]});`)(...names.map((n) => seen[n]));
}
/* a readonly colour, in ControlCenterView.qml, as r, g, b */
function color(name) {
  const m = new RegExp(`readonly property color ${name}: "#([0-9a-f]{6})"`, 'i').exec(QML);
  assert.ok(m, `ControlCenterView.qml has the colour ${name}`);
  return [0, 2, 4].map((i) => parseInt(m[1].slice(i, i + 2), 16));
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
  assert.equal(prop('gridRows'), define('CC_ROWS'), 'as many rows');
});

test('qml sizes: the overlay\'s gallery is Home+\'s', () => {
  for (const [q, c] of [['galW', 'GAL_W'], ['galPad', 'GAL_PAD'], ['galHead', 'GAL_HEAD'], ['galGroup', 'GAL_GROUP'], ['galGroupTop', 'GAL_GROUP_TOP'],
    ['galBtn', 'GAL_BTN'], ['galRow', 'GAL_ROW'], ['galNamePx', 'GAL_NAME_PX'], ['galNameGap', 'GAL_NAME_GAP']]) {
    assert.equal(prop(q), define(c), `${q} is ${c}`);
  }
});

test('qml sizes: the overlay\'s lists and sheets are Home+\'s', () => {
  for (const [q, c] of [['listRowH', 'CC_LIST_ROW_H'], ['listGap', 'CC_LIST_GAP'], ['listHead', 'CC_LIST_HEAD'], ['listPx', 'CC_LIST_PX'],
    ['detailPx', 'CC_DETAIL_PX'], ['rowA', 'CC_ROW_A'], ['detailA', 'CC_DETAIL_A'], ['problemPx', 'CC_PROBLEM_PX'],
    ['sheetW', 'SHEET_W'], ['sheetPad', 'PAD'], ['sheetBtn', 'SHEET_BH'], ['sheetGap', 'SHEET_GAP'], ['sheetTitlePx', 'SHEET_TITLE_PX'],
    ['sheetBodyPx', 'SHEET_BODY_PX'], ['sheetButtonPx', 'SHEET_BUTTON_PX'], ['sheetTitleA', 'SHEET_TITLE_A'], ['sheetBodyA', 'SHEET_BODY_A'],
    ['sheetLabelA', 'SHEET_LABEL_A']]) {
    assert.equal(prop(q), define(c), `${q} is ${c}`);
  }
  assert.match(C, /"Picture Mode" : "Inputs"/, 'Home+ calls its list Picture Mode');
  assert.match(QML, /"Picture Mode" : "Inputs"/, 'and so does the overlay');
});

test('qml sizes: one red for what can\'t be undone, on glass and on white, in both', () => {
  assert.deepEqual(color('red'), defineRgb('RED_ON_GLASS'));
  assert.deepEqual(color('redOnWhite'), defineRgb('RED_ON_WHITE'));
  const reds = QML.match(/"#[0-9a-f]{6}"/gi).map((h) => h.slice(2, 8).toLowerCase()).filter((h) => {
    const [r, g, b] = [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16));
    return r > 180 && g < 120 && b < 120;
  });
  assert.deepEqual([...new Set(reds)].sort(), ['d72828', 'ff5a50'], 'no other red in the overlay');
});
