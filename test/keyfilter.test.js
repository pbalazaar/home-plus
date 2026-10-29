/* Home+'s key filter in LG's compositor (service/keyfilter.js): the remote's Home, Settings and Input keys
 * without LG Input Hook. The filter it writes, run in a stand-in for the compositor's key filter shell;
 * the list it keeps it in, against a stand-in configd; then the real service on the preview harness's
 * stand-in TV, with no LG Input Hook. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { URL } = require('url');
const { EventEmitter } = require('events');
const createKeyFilter = require('../service/keyfilter');
const { rmSync } = require('../service/remove'); /* (runtime.test.js runs this file on the TV's Node too: no fs.rmSync before 14.14) */
const { startService, until, last } = require('./helpers');

const { filterSource, withFilter, KEY_FILTERS } = createKeyFilter;
const KF = '/usr/lib/qt5/qml/KeyFilters/';
/* the C3's list (webOS 23, configd's base layer), as getConfigs gave it on 2026-10-29 */
const LG_LIST = [
  ['preProcess', 'handleKeyBlockByApp'], ['systemUi', 'handleScreenSaver'], ['appLaunch', 'handleHotelmode'],
  ['appLaunch', 'handleAccessibility'], ['appLaunch', 'handleAvReset'], ['appLaunch', 'handleFreeSyncInfo'],
  ['appLaunch', 'handleFactoryReset'], ['appLaunch', 'handlePanelUTTUsbDownload'], ['appLaunch', 'handleExternalVendorOverlays'],
  ['appLaunch', 'handleLowSpeedDataBroadcast'], ['systemUi', 'handleGamepadKeys'], ['systemUi', 'handleSystemKeys'],
  ['appLaunch', 'handleHotkeys'], ['appLaunch', 'handleCPHotkeys'], ['appLaunch', 'handleFactoryKeys'],
  ['appLaunch', 'handleTvhotkeyAppLaunch'], ['appLaunch', 'handleLocalKeys'], ['systemUi', 'handleMagicNumber']
].map(([file, handler]) => ({ handler, file: `${KF}${file}.js` }));
const handlers = (list) => list.map((f) => f.handler);
const ID = 'com.github.pbalazaar.homeplus';
const API = 'http://127.0.0.1:47823/api/key';
const KEY = { home: 0x01000053, settings: 0x0200001b, input: 0x0200002f, up: 0x01000013 }; /* (any distinct numbers do) */
const tick = () => new Promise((resolve) => setImmediate(resolve));

/* The compositor's key filter shell, as LG's StarfishKeyFilterShell.js gives one: its helpers, and the
 * filter included into it. send(): what lunaCall sent, each with its callback; xhrs: the requests made
 * (answer(status) answers one); timers: genericTimer's (fire() runs one). */
function shell(source, helpers = {}) {
  const sent = [], xhrs = [], timers = [];
  class Request {
    constructor() { this.readyState = 0; this.status = 0; this.headers = {}; xhrs.push(this); }
    open(method, url) { this.method = method; this.url = url; this.readyState = 1; }
    setRequestHeader(name, value) { this.headers[name] = value; }
    send(body) { this.body = body; }
    abort() { this.aborted = true; }
    answer(status) { this.status = status; this.readyState = 4; this.onreadystatechange(); }
  }
  const ctx = vm.createContext(Object.assign({
    Qt: { Key_Super_L: KEY.home, Key_Up: KEY.up },
    WebOS: { Key_webOS_Settings: KEY.settings, Key_webOS_InputSource: KEY.input },
    KeyPolicy: { NotAccepted: 0, Accepted: 1, NextPolicy: 2 },
    lunaCall: { appId: 'com.webos.starfish.keyfilters', send: (service, method, p, cb) => sent.push({ uri: service + method, params: JSON.parse(p), cb }) },
    genericTimer: {
      set: (ms, fn) => timers.push({ ms, fn, on: true }),
      cancel: (id) => { timers[id - 1].on = false; }
    },
    XMLHttpRequest: Request,
    bootManager: { bootFirstUse: false }, systemPropertyService: { powerOnlyMode: false },
    modalAlert: { visible: false }, settingsService: { disableHome: false }
  }, helpers));
  vm.runInContext(source, ctx);
  ctx.init();
  const press = (key, pressed = true, repeat = false) => ctx.handleHomePlusKeys(key, pressed, repeat);
  const fire = () => timers.filter((t) => t.on).forEach((t) => { t.on = false; t.fn(); });
  return { ctx, sent, xhrs, timers, press, fire };
}
const source = (keys) => filterSource({ keys, api: API, token: 'secret', appId: ID, lgHome: 'com.webos.app.home' });
const Accepted = 1, NextPolicy = 2; /* (the shell's KeyPolicy) */

test('key filter: in the list before LG\'s Home key, in place of an older one, and out again', () => {
  const ours = withFilter(LG_LIST, '/var/lib/homeplus/keyfilter-a.js');
  assert.deepEqual(handlers(ours).slice(10, 13), ['handleGamepadKeys', 'handleHomePlusKeys', 'handleSystemKeys']);
  assert.equal(ours.length, LG_LIST.length + 1);
  const again = withFilter(ours, '/var/lib/homeplus/keyfilter-b.js');
  assert.deepEqual(again.filter((f) => f.handler === 'handleHomePlusKeys'), [{ file: '/var/lib/homeplus/keyfilter-b.js', handler: 'handleHomePlusKeys' }]);
  assert.deepEqual(withFilter(again, null), LG_LIST, 'LG\'s own, in its order');
  assert.equal(withFilter(LG_LIST.filter((f) => f.handler !== 'handleSystemKeys'), '/x.js'), null, 'a list Home+ doesn\'t know');
  assert.equal(withFilter(undefined, '/x.js'), null);
});

test('key filter: keys it wasn\'t asked to take go on to LG\'s filters', () => {
  const t = shell(source({ home: false, settings: true, input: false }));
  assert.equal(t.press(KEY.home), NextPolicy);
  assert.equal(t.press(KEY.input), NextPolicy);
  assert.equal(t.press(KEY.up), NextPolicy);
  assert.deepEqual(t.sent, []);
  assert.deepEqual(t.xhrs, []);
});

test('key filter: Home opens Home+, or LG\'s home screen when Home+ doesn\'t open', () => {
  const t = shell(source({ home: true, settings: false, input: false }));
  assert.equal(t.press(KEY.home), Accepted);
  assert.equal(t.press(KEY.home, true, true), Accepted, 'held: nothing more');
  assert.equal(t.press(KEY.home, false), Accepted, 'let go: nothing more');
  assert.equal(t.sent.length, 1);
  assert.deepEqual(t.sent[0].params, { id: ID });
  assert.equal(t.sent[0].uri, 'com.webos.applicationManager/launch');
  t.sent[0].cb('{"returnValue": true}'); /* (lunaCall's answers are JSON text) */
  assert.equal(t.sent.length, 1, 'Home+ opened');
  t.press(KEY.home);
  t.sent[1].cb('{"returnValue": false, "errorCode": -101, "errorText": "not found"}'); /* Home+ is gone */
  assert.deepEqual(last(t.sent).params, { id: 'com.webos.app.home' });
  assert.equal(t.sent.length, 3);
});

test('key filter: Settings and Input ask Home+\'s service, with its token', () => {
  const t = shell(source({ home: false, settings: true, input: true }));
  assert.equal(t.press(KEY.settings), Accepted);
  assert.equal(t.press(KEY.settings, false), Accepted, 'let go: nothing more');
  assert.equal(t.press(KEY.input), Accepted);
  assert.equal(t.xhrs.length, 2);
  for (const [i, key] of [[0, 'settings'], [1, 'input']]) {
    const r = t.xhrs[i];
    assert.equal(r.method, 'POST');
    assert.equal(r.url, API);
    assert.equal(r.headers['X-Homeplus-Token'], 'secret');
    assert.deepEqual(JSON.parse(r.body), { key });
    r.answer(200);
  }
  t.fire(); /* (the 2-s timers, cancelled by the answers) */
  assert.deepEqual(t.sent, [], 'nothing of LG\'s');
});

test('key filter: LG\'s Quick Settings or Input picker when the service doesn\'t answer, once', () => {
  const t = shell(source({ home: false, settings: true, input: true }));
  t.press(KEY.settings);
  t.xhrs[0].answer(0); /* (nothing listening: Home+ is gone) */
  t.fire();
  assert.deepEqual(t.sent.map((s) => [s.uri, s.params]), [['com.webos.service.systemUIManager/launch', { appId: 'com.webos.app.quicksettings' }]]);
  t.press(KEY.input);
  t.fire(); /* 2 s and no answer */
  t.xhrs[1].answer(200); /* (late) */
  assert.deepEqual(last(t.sent).params, { appId: 'com.webos.app.quickinputpicker' });
  assert.equal(t.xhrs[1].aborted, true);
  assert.equal(t.sent.length, 2);
});

test('key filter: keys LG would ignore just now go on to LG\'s own filters', () => {
  const firstUse = shell(source({ home: true, settings: true, input: true }), { bootManager: { bootFirstUse: true } });
  for (const k of [KEY.home, KEY.settings, KEY.input]) assert.equal(firstUse.press(k), NextPolicy, 'in LG\'s first-time setup');
  const powerOnly = shell(source({ home: true, settings: false, input: false }), { bootManager: { bootFirstUse: true }, systemPropertyService: { powerOnlyMode: true } });
  assert.equal(powerOnly.press(KEY.home), Accepted, 'LG takes keys then too');
  const alert = shell(source({ home: true, settings: true, input: false }), { modalAlert: { visible: true } });
  assert.equal(alert.press(KEY.home), NextPolicy, 'under an alert, LG ignores Home');
  assert.equal(alert.press(KEY.settings), Accepted);
  assert.equal(shell(source({ home: true, settings: false, input: false }), { settingsService: { disableHome: true } }).press(KEY.home), NextPolicy);
  const bare = shell(source({ home: true, settings: false, input: false }), { bootManager: undefined, modalAlert: undefined });
  assert.equal(bare.press(KEY.home), Accepted, 'helpers LG\'s shell didn\'t give: the key is taken');
});

/* configd as webOS OSE's: a volatile value goes over the layers' own, a null takes it away, each change
 * goes to the subscribers, and a reconfigure (its layers read again) drops volatile values. */
function stubConfigd(list = LG_LIST, { refuse = false } = {}) {
  const subs = [], sets = [];
  let held; /* the volatile value: undefined when there's none */
  const now = () => (held !== undefined ? held : list);
  const say = () => subs.forEach((s) => s.emit('response', { payload: { configs: { [KEY_FILTERS]: now() } } }));
  return {
    sets,
    now,
    answer: () => subs.forEach((s) => s.emit('response', { payload: { returnValue: true, subscribed: true, configs: { [KEY_FILTERS]: now() } } })),
    reconfigure: () => { held = undefined; say(); },
    gone: () => { subs.length = 0; }, /* the services subscribed so far stopped */
    luna: async (uri, params) => {
      if (!uri.endsWith('/setConfigs')) return { returnValue: true };
      sets.push(params);
      if (refuse) return { returnValue: false, errorText: 'Denied method call "setConfigs" for category "/"' };
      held = params.configs[KEY_FILTERS] === null ? undefined : params.configs[KEY_FILTERS];
      setImmediate(say);
      return { returnValue: true };
    },
    subscribe: (uri, params) => {
      assert.equal(uri, 'luna://com.webos.service.config/getConfigs');
      assert.deepEqual(params, { configNames: [KEY_FILTERS], subscribe: true });
      const s = new EventEmitter();
      subs.push(s);
      return s;
    }
  };
}

function keyFilter(configd, dir, token = 't1') {
  const logged = [];
  const kf = createKeyFilter({ luna: configd.luna, subscribe: configd.subscribe, log: (...p) => logged.push(p.join(' ')), dir: () => dir,
    write: (f, text) => fs.writeFileSync(f, text, { mode: 0o600 }), api: API, token: () => token, appId: ID, lgHome: 'com.webos.app.home' });
  kf.start();
  return Object.assign(kf, { logged });
}
const files = (dir) => fs.readdirSync(dir).filter((f) => f.startsWith('keyfilter-'));
const tempDir = () => fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-keyfilter-'));

test('key filter: nothing until configd says what the list is; then Home+\'s goes in, before LG\'s Home key', async () => {
  const configd = stubConfigd(), dir = tempDir();
  try {
    const kf = keyFilter(configd, dir);
    await kf.set({ settings: true });
    assert.equal(kf.available(), false);
    assert.deepEqual(configd.sets, []);
    assert.deepEqual(files(dir), []);
    configd.answer();
    await tick();
    await kf.set({ settings: true });
    assert.equal(kf.available(), true);
    assert.equal(configd.sets.length, 1);
    assert.equal(configd.sets[0].volatile, true, 'in configd\'s memory only: a reboot clears it');
    const list = configd.now(), ours = list[11];
    assert.deepEqual(handlers(list).slice(10, 13), ['handleGamepadKeys', 'handleHomePlusKeys', 'handleSystemKeys']);
    assert.equal(path.dirname(ours.file), dir);
    assert.deepEqual(files(dir), [path.basename(ours.file)]);
    const text = fs.readFileSync(ours.file, 'utf8');
    assert.match(text, /var homeplusKeys = \{"home":false,"settings":true,"input":false\};/);
    assert.match(text, /"X-Homeplus-Token", "t1"/);
    if (process.platform !== 'win32') assert.equal(fs.statSync(ours.file).mode & 0o777, 0o600, 'the token\'s in it: root\'s alone');
    assert.equal(kf.takes('settings'), true);
    assert.equal(kf.takes('home'), false);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('key filter: the same keys change nothing; others are a new file in place of the old', async () => {
  const configd = stubConfigd(), dir = tempDir();
  try {
    const kf = keyFilter(configd, dir);
    configd.answer();
    await kf.set({ settings: true });
    await tick();
    await kf.set({ settings: true });
    assert.equal(configd.sets.length, 1);
    const before = files(dir);
    await kf.set({ settings: true, home: true });
    await tick();
    assert.equal(configd.sets.length, 2);
    assert.equal(configd.now().filter((f) => f.handler === 'handleHomePlusKeys').length, 1);
    assert.notDeepEqual(files(dir), before);
    assert.equal(files(dir).length, 1, 'the old file is gone');
    assert.equal(kf.takes('home'), true);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('key filter: put back when configd reads its layers again', async () => {
  const configd = stubConfigd(), dir = tempDir();
  try {
    const kf = keyFilter(configd, dir);
    configd.answer();
    await kf.set({ input: true });
    const placed = configd.now();
    configd.reconfigure(); /* (volatile values dropped: LG's own list again) */
    await tick();
    await kf.set({ input: true });
    assert.equal(configd.sets.length, 2);
    assert.deepEqual(configd.now(), placed);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('key filter: no keys, and Remove Home+: out of the list, LG\'s own back, its files gone', async () => {
  const configd = stubConfigd(), dir = tempDir();
  try {
    const kf = keyFilter(configd, dir);
    configd.answer();
    await kf.set({ home: true });
    await kf.set({});
    assert.equal(last(configd.sets).configs[KEY_FILTERS], null, 'the volatile value taken away');
    assert.deepEqual(configd.now(), LG_LIST);
    assert.deepEqual(files(dir), []);
    assert.equal(kf.takes('home'), false);
    await kf.set({ settings: true });
    assert.equal(await kf.remove(), true);
    assert.deepEqual(configd.now(), LG_LIST);
    assert.deepEqual(files(dir), []);
    assert.match(kf.logged.join('\n'), /key filter: off/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('key filter: a list without LG\'s Home key filter, or configd refusing, takes nothing', async () => {
  const dir = tempDir();
  try {
    const unknown = stubConfigd(LG_LIST.filter((f) => f.handler !== 'handleSystemKeys'));
    const a = keyFilter(unknown, dir);
    unknown.answer();
    await a.set({ home: true, settings: true });
    assert.equal(a.available(), false);
    assert.deepEqual(unknown.sets, []);

    const refusing = stubConfigd(LG_LIST, { refuse: true });
    const b = keyFilter(refusing, dir);
    refusing.answer();
    await b.set({ settings: true });
    assert.equal(b.takes('settings'), false);
    assert.deepEqual(refusing.now(), LG_LIST);
    assert.match(b.logged.join('\n'), /configd refused it: Denied method call/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('key filter: a service started again (a new token) puts its own in place of the last one\'s', async () => {
  const configd = stubConfigd(), dir = tempDir();
  try {
    const before = keyFilter(configd, dir, 't1');
    configd.answer();
    await before.set({ settings: true });
    configd.gone(); /* (an update stops the service; its filter stays in configd) */
    const after = keyFilter(configd, dir, 't2');
    configd.answer();
    await tick();
    await after.set({ settings: true });
    const ours = configd.now().filter((f) => f.handler === 'handleHomePlusKeys');
    assert.equal(ours.length, 1);
    assert.match(fs.readFileSync(ours[0].file, 'utf8'), /"X-Homeplus-Token", "t2"/);
    assert.deepEqual(files(dir), [path.basename(ours[0].file)]);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

/* The real service with no LG Input Hook, on a TV whose configd has the C3's list */
test('key filter: without LG Input Hook, Settings offers the buttons and No Flash, and the filter takes the keys', async () => {
  const dir = tempDir();
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const lunaLog = path.join(dir, 'luna.log');
  const sets = () => fs.readFileSync(lunaLog, 'utf8').trim().split('\n').map((l) => JSON.parse(l)).filter((c) => c.uri.endsWith('/setConfigs'));
  const svc = await startService(dir, {
    HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'no-lginputhook', 'keybinds.json'),
    HOMEPLUS_APP_DIR: path.join(dir, 'app'), HOMEPLUS_INITD_DIR: path.join(dir, 'init.d'),
    HOMEPLUS_PREVIEW_LUNA_LOG: lunaLog,
    HOMEPLUS_PREVIEW_SUBSCRIPTIONS: JSON.stringify({ getConfigs: [[100, { returnValue: true, subscribed: true, configs: { [KEY_FILTERS]: LG_LIST } }]] })
  });
  try {
    await until(async () => (await svc.call('GET', '/api/home')).json.settingsKey.available, 8000, 'the key filter');
    const home = (await svc.call('GET', '/api/home')).json;
    assert.equal(home.noFlashAvailable, true, 'LG\'s home can\'t be redirected here, and the key filter can take Home');
    assert.deepEqual(home.inputKey, { available: true, on: false });

    const on = (await svc.call('POST', '/api/home', { body: { settingsKey: true } })).json;
    assert.deepEqual(on.settingsKey, { available: true, on: true });
    const ours = (list) => list.find((f) => f.handler === 'handleHomePlusKeys');
    const placed = last(sets()).params;
    assert.equal(placed.volatile, true);
    const file = ours(placed.configs[KEY_FILTERS]).file;
    assert.equal(path.dirname(file), path.join(dir, 'config'));
    assert.deepEqual(handlers(placed.configs[KEY_FILTERS]).slice(10, 13), ['handleGamepadKeys', 'handleHomePlusKeys', 'handleSystemKeys']);

    /* the Settings key, through the filter the service wrote, to the service itself */
    const t = shell(fs.readFileSync(file, 'utf8'));
    t.ctx.XMLHttpRequest = class {
      open(method, url) { this.method = method; this.url = url; this.headers = {}; }
      setRequestHeader(name, value) { this.headers[name] = value; }
      send(body) {
        const u = new URL(this.url); /* (http.request(url, options) is Node 10.9's) */
        const req = http.request({ hostname: u.hostname, port: u.port, path: u.pathname, method: this.method, headers: this.headers }, (res) => {
          res.resume();
          res.on('end', () => { this.status = res.statusCode; this.readyState = 4; this.onreadystatechange(); });
        });
        req.end(body);
      }
      abort() {}
    };
    let answered = 0;
    t.ctx.genericTimer = { set: () => 1, cancel: () => { answered++; } };
    assert.equal(t.press(KEY.settings), Accepted);
    await until(() => answered === 1, 5000, 'the service to answer the filter');
    assert.deepEqual(t.sent, [], 'it did: nothing of LG\'s from the filter');

    const noFlash = (await svc.call('POST', '/api/home', { body: { mode: 'redirect', asked: true } })).json;
    assert.equal(noFlash.noFlashActive, true);
    assert.match(fs.readFileSync(ours(last(sets()).params.configs[KEY_FILTERS]).file, 'utf8'), /"home":true,"settings":true/);

    const removed = (await svc.call('POST', '/api/uninstall')).json;
    assert.equal(removed.keyFilter, true);
    assert.equal(last(sets()).params.configs[KEY_FILTERS], null, 'out of the compositor\'s list');
  } finally {
    await svc.stop();
    rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
  }
});
