/* The Control Center over other apps (native/qml) is drawn as Home+'s own (native/src), and
 * its sizes are copies of the launcher's: these keep the two in step. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
/* the launcher's own code: main.c, home.h and the files that include it (not the libraries: render.c, net.c, ...) */
const SRC = path.join(ROOT, 'native/src');
const C = fs.readdirSync(SRC).filter((f) => /\.[ch]$/.test(f)).sort()
  .map((f) => [f, fs.readFileSync(path.join(SRC, f), 'utf8')])
  .filter(([f, src]) => f === 'main.c' || f === 'home.h' || src.includes('#include "home.h"'))
  .map(([, src]) => src).join('\n');
const QML = fs.readFileSync(path.join(ROOT, 'native/qml/ControlCenterView.qml'), 'utf8');
const SETTINGS = fs.readFileSync(path.join(ROOT, 'native/qml/SettingsPanel.qml'), 'utf8');
const RECENTS = fs.readFileSync(path.join(ROOT, 'native/qml/RecentsStrip.qml'), 'utf8');
const PAGES = fs.readFileSync(path.join(ROOT, 'service/settings.js'), 'utf8');
/* the launcher's words as they show: its UTF-8 written out as bytes (\xE2\x80\x99) */
const WORDS = C.replace(/(?:\\x[0-9A-F]{2})+/g, (m) => Buffer.from(m.split('\\x').slice(1).map((h) => parseInt(h, 16))).toString('utf8'));

/* a #define's number, in the launcher */
function define(name) {
  const m = new RegExp(`^#define ${name} (\\d+)\\b`, 'm').exec(C);
  assert.ok(m, `the launcher defines ${name}`);
  return Number(m[1]);
}
/* a #define's colour, in the launcher: "255, 90, 80" */
function defineRgb(name) {
  const m = new RegExp(`^#define ${name} (\\d+), (\\d+), (\\d+)\\b`, 'm').exec(C);
  assert.ok(m, `the launcher defines ${name} as r, g, b`);
  return m.slice(1).map(Number);
}
/* a readonly int's value, in ControlCenterView.qml or another of them, worked out from the ones before it */
function prop(name, seen = {}, qml = QML) {
  const m = new RegExp(`readonly property int ${name}: ([^\\n/]+)`).exec(qml);
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
  assert.match(C, /(?:static )?float cc_grid_y\(void\) \{ return TOP_Y \+ BAR \+ 26; \}/, 'cc_grid_y() as the overlay has it');
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
    ['detailPx', 'CC_DETAIL_PX'], ['sheetW', 'SHEET_W'], ['sheetPad', 'PAD'], ['sheetBtn', 'SHEET_BH'], ['sheetGap', 'SHEET_GAP'],
    ['sheetTitlePx', 'SHEET_TITLE_PX'], ['sheetBodyPx', 'SHEET_BODY_PX'], ['sheetButtonPx', 'SHEET_BUTTON_PX'], ['hintPx', 'HINT_PX'],
    ['textStatic', 'TEXT_STATIC'], ['textSecondary', 'TEXT_SECONDARY'], ['textTertiary', 'TEXT_TERTIARY']]) {
    assert.equal(prop(q), define(c), `${q} is ${c}`);
  }
  assert.match(C, /"Picture Mode" : "Inputs"/, 'Home+ calls its list Picture Mode');
  assert.match(QML, /"Picture Mode" : "Inputs"/, 'and so does the overlay');
});

test('qml sizes: the overlay\'s Settings is Home+\'s two panes', () => {
  for (const [q, c] of [['splitW', 'SPLIT_W'], ['splitH', 'SPLIT_H'], ['splitSide', 'SPLIT_SIDE'], ['splitCat', 'SPLIT_CAT'], ['splitCatGap', 'SPLIT_CAT_GAP'],
    ['splitRow', 'SPLIT_ROW'], ['splitGap', 'SPLIT_GAP'], ['splitTitlePx', 'SPLIT_TITLE_PX'], ['splitDescPx', 'SPLIT_DESC_PX'], ['splitHintPx', 'SPLIT_HINT_PX'],
    ['swatchD', 'SWATCH_D'], ['swatchPitch', 'SWATCH_PITCH']]) {
    assert.equal(prop(q, {}, SETTINGS), define(c), `${q} is ${c}`);
  }
  assert.match(C, new RegExp(`text_render\\(S\\.f_medium, ${prop('catPx', {}, SETTINGS)}, CATS\\[i\\]\\.name`), 'its list\'s words');
});

test('qml sizes: the pointer takes over as in Home+', () => {
  assert.equal(prop('ptrTakePx'), define('PTR_TAKE_PX'));
});

/* every page, with every row it can have */
async function allPages() {
  const settings = require(path.join(ROOT, 'service/settings'))({ /* (by path: this file isn't one of those that run on the TV's Node) */
    log: () => {}, readConfig: () => ({ prefs: { iconLook: 'tinted' }, layout: { hidden: ['a'] } }), setPrefs: () => {}, setHome: async () => {},
    home: async () => ({ mode: 'watch', noFlashActive: false, noFlashAvailable: true, settingsKey: { available: true, on: true }, inputKey: { available: true, on: true }, piccap: true }),
    apps: async () => [{ id: 'stremio', title: 'Stremio' }, { id: 'jellyfin', title: 'Jellyfin' }, { id: 'plex', title: 'Plex' }], tvApps: async () => [],
    accounts: () => ({ stremio: {}, trakt: {}, jellyfin: {}, plex: {}, tmdb: {} }), version: '1', elevated: () => true
  });
  return (await settings.state()).pages;
}

test('copy: Settings over other apps says what Home+ says', async () => {
  let rows = 0;
  for (const page of await allPages()) {
    for (const t of [page.name, page.about]) assert.ok(WORDS.includes(`"${t}"`), `Home+ says "${t}"`);
    for (const line of (page.body || '').split('\n').filter((l) => l && !l.startsWith('Home+ 1,'))) assert.ok(WORDS.includes(line), `Home+ says "${line}"`);
    for (const r of page.rows) {
      rows++;
      assert.ok(WORDS.includes(`"${r.label}"`), `Home+ says "${r.label}"`);
      if (r.desc) assert.ok(WORDS.includes(`"${r.desc}"`), `Home+ says "${r.desc}"`);
      if (r.value && !/\d/.test(r.value)) assert.ok(WORDS.includes(`"${r.value}"`), `Home+ says "${r.value}"`);
    }
  }
  assert.ok(rows >= 25, `it found the rows: ${rows}`);
});

test('copy: the keys\' hints and Recents\' words are Home+\'s', () => {
  for (const hint of [...SETTINGS.matchAll(/"([^"]*Back [^"]*)"/g)].map((m) => m[1]).filter((h) => !h.includes('Home+'))) {
    assert.ok(WORDS.includes(`"${hint}"`), `Home+ says "${hint}"`);
  }
  for (const words of ['OK opens  ·  Up closes', 'Close All', 'No apps running', 'Couldn’t load recent apps', 'Couldn’t close that app']) {
    assert.ok(RECENTS.includes(`"${words}"`), `the strip says "${words}"`);
    assert.ok(WORDS.includes(words), `Home+ says "${words}"`);
  }
});

test('qml sizes: Settings\' choices are the launcher\'s', () => {
  const list = (src, re) => re.exec(src)[1].split(',').map(Number);
  assert.deepEqual(list(PAGES, /DIM = \[([^\]]+)\]/), list(C, /dim\[\] = \{ ([^}]+) \}/));
  assert.deepEqual(list(PAGES, /OFF = \[([^\]]+)\]/), list(C, /off\[\] = \{ ([^}]+) \}/));
  assert.equal(Number(/DIM_DEFAULT = (\d+)/.exec(PAGES)[1]), Number(/#define IDLE_DIM_MS \((\d+) \*/.exec(C)[1]));
  assert.equal(Number(/OFF_DEFAULT = (\d+)/.exec(PAGES)[1]), Number(/#define IDLE_OFF_MS \((\d+) \*/.exec(C)[1]));
  const tints = [...C.matchAll(/\{ "(\w+)", \{ (\d+), (\d+), (\d+) \} \}/g)].map((m) => [m[1], '#' + m.slice(2).map((n) => Number(n).toString(16).padStart(2, '0')).join('')]);
  assert.equal(tints.length, 10, 'Home+ has ten');
  for (const [name, hex] of tints) assert.ok(PAGES.includes(`['${name}', '${hex}']`), `${name} is ${hex}`);
  const clients = [...C.matchAll(/\{ "([\w.]+)", SRC_(JELLYFIN|PLEX) \}/g)].map((m) => [m[1], m[2].toLowerCase()]);
  assert.equal(clients.length, 5, 'the other apps it knows');
  for (const [id, src] of clients) assert.ok(PAGES.includes(`'${id}': '${src}'`), `${id} is ${src}`);
});

test('qml sizes: the editing hints say the same in both', () => {
  const hints = [...C.matchAll(/S\.cc\.(?:edit_hint|move_hint|no_room) = text_render\(S\.f_medium, HINT_PX, "([^"]+)"/g)]
    .map((m) => m[1].replace(/\\xC2\\xB7/g, '·'));
  assert.equal(hints.length, 3, 'Home+ has three');
  for (const h of hints) assert.ok(QML.includes(`"${h}"`), `the overlay says "${h}"`);
});

/* what's on screen: the string literals of the launcher and the overlay that aren't JSON, paths or formats' innards */
function onScreen(src) {
  return [...src.matchAll(/"((?:[^"\\\n]|\\.)*)"/g)].map((m) => m[1]).filter((t) => /[A-Za-z]{2,} [A-Za-z]/.test(t) && !/[{}\/]|\\"/.test(t));
}

test('copy: apostrophes are curly, and no room is "No room"', () => {
  for (const [file, src] of [['the launcher', C], ['ControlCenterView.qml', QML], ['SettingsPanel.qml', SETTINGS], ['RecentsStrip.qml', RECENTS], ['settings.js', PAGES]]) {
    const straight = onScreen(src).filter((t) => /[A-Za-z]'[a-z]/.test(t));
    assert.deepEqual(straight, [], `${file}: straight apostrophes`);
    assert.doesNotMatch(src, /No Room/, `${file}: "No room", as the hints are in sentence case`);
  }
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
