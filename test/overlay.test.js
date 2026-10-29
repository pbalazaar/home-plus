/* The Control Center over other apps: the Settings key, with an app in front, opens it as a system
 * UI (systemUIManager/launch) with the controls' state; while it's open the service grabs frames of
 * the video for its glass; and it asks the service, over the API, to open what it can't. The real
 * service.js on the preview harness's stand-in webOS, with an app folder that has the QML. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { fileURLToPath } = require('url');
const { until, sleep, startService } = require('./helpers');

const ROOT = path.join(__dirname, '..');
const windows = process.platform === 'win32' && 'the TV\'s: file URLs of Unix paths, links and Unix modes'; /* a reason to skip */
let svc, port, token, dir;
const call = (...args) => svc.call(...args);

const lines = (file) => (fs.existsSync(path.join(dir, file)) ? fs.readFileSync(path.join(dir, file), 'utf8') : '')
  .split('\n').filter(Boolean).map((l) => JSON.parse(l));
const lunaLog = () => lines('luna.log');
const calls = (uri) => lunaLog().filter((c) => c.uri === uri);
const SHOT = 'luna://com.webos.service.capture/executeOneShot', LAUNCH = 'luna://com.webos.service.systemUIManager/launch';
// the service's waits here (on the TV: longer): captures start GAP ms apart at most (on the TV: a third of
// a second); one gives no frame after GIVE_UP ms (3 s); RETRY are the waits while the video can't be
// captured (1.5, 3, then 5 s); luna gives up on a call after LUNA_MS (15 s); PicCap starts again AGAIN_MS
// after a close (2 s)
const GAP = 200, GIVE_UP = 400, RETRY = [250, 500, 750], LUNA_MS = 1000, AGAIN_MS = 300;

// the TV's captures asked for since `since`, in order: each one's method, file, and when it was asked
// for and answered (the stand-in's captures.log; none: not yet, or never)
function capturesSince(since) {
  const ends = new Map(lines('captures.log').map((c) => [c.path, c.end]));
  return calls(SHOT).filter((c) => c.t >= since).map((c) => ({ method: c.params.method, file: c.params.path, start: c.t, end: ends.get(c.params.path) }));
}
const videoSince = (since) => capturesSince(since).filter((c) => c.method === 'VIDEO');
// none asked for before the one before it had answered (one never answered: until luna gave up on it)
function oneAtATime(list) {
  for (let i = 1; i < list.length; i++) {
    const last = list[i - 1], free = last.end === undefined ? last.start + LUNA_MS : last.end;
    assert.ok(list[i].start >= free, `a ${list[i].method} capture asked for ${list[i].start - last.start} ms after a ${last.method} one that answered after ${free - last.start} ms`);
  }
}
const ask = async (after) => (await call('POST', '/api/overlay', { body: { event: 'frame', after } })).json;
const framesFolder = () => path.dirname(fileURLToPath(calls(LAUNCH).at(-1).params.params.frame));

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-overlay-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix,youtube|YouTube');
  const app = path.join(dir, 'app'); /* the installed app folder, with the Control Center's QML */
  fs.mkdirSync(path.join(app, 'qml'), { recursive: true });
  for (const f of ['ControlCenter.qml', 'ControlCenterView.qml']) fs.copyFileSync(path.join(ROOT, 'native/qml', f), path.join(app, 'qml', f));
  fs.symlinkSync('qml', path.join(app, 'qml-old')); /* an older version's link */
  fs.mkdirSync(path.join(dir, 'frames')); /* where the frames' folders go (on the TV, /tmp) */
  svc = await startService(dir, {
    HOMEPLUS_APP_DIR: app,
    HOMEPLUS_FRAMES_DIR: path.join(dir, 'frames'),
    HOMEPLUS_PREVIEW_LUNA_LOG: path.join(dir, 'luna.log'),
    HOMEPLUS_PREVIEW_FOREGROUND: 'netflix', /* an app is in front */
    HOMEPLUS_PREVIEW_AMBIENT: '1', /* with PicCap, capturing */
    HOMEPLUS_PICCAP_PAUSED: path.join(dir, 'piccap-paused'),
    HOMEPLUS_PICCAP_AGAIN_MS: String(AGAIN_MS),
    HOMEPLUS_CAPTURE_RATE: String(1000 / GAP),
    HOMEPLUS_CAPTURE_TIMEOUT_MS: String(GIVE_UP),
    HOMEPLUS_CAPTURE_RETRY_MS: RETRY.join(),
    HOMEPLUS_LUNA_TIMEOUT_MS: String(LUNA_MS)
  });
  ({ port, token } = svc);
});

test.after(async () => {
  if (svc) await svc.stop();
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

test('overlay: the Settings key opens it over the app in front, with the controls, the API and the frames', { skip: windows }, async () => {
  const r = await call('POST', '/api/key', { body: { key: 'settings' } });
  assert.deepEqual(r.json, { opened: 'overlay' });
  const launch = calls('luna://com.webos.service.systemUIManager/launch').at(-1);
  assert.equal(launch.params.appId, 'com.github.pbalazaar.homeplus.cc');
  assert.equal(launch.params.windowType, '_WEBOS_SYSTEM_UI_TYPE_OVERLAY');
  const m = /^file:\/\/(.+)\/(qml-[0-9a-z]+)\/ControlCenter\.qml$/.exec(launch.params.main);
  assert.ok(m, `opened from a link named for the files' time: ${launch.params.main}`);
  assert.equal(fs.readlinkSync(path.join(m[1], m[2])), 'qml');
  assert.ok(!fs.existsSync(path.join(m[1], 'qml-old')), "an older version's link is gone");
  const p = launch.params.params;
  assert.ok(Array.isArray(p.state.controls) && p.state.controls.length, 'the controls on show');
  assert.equal(p.token, token, 'the API token: it can only reach the service over the API');
  assert.equal(p.port, port);
  const fm = new RegExp(`^file://${path.join(dir, 'frames')}/(homeplus-cc-[^/]+)/frame\\.png$`).exec(p.frame);
  assert.ok(fm, `a folder of its own for the frames, with a name nobody can guess: ${p.frame}`);
  const frames = path.join(dir, 'frames', fm[1]);
  await until(() => calls('luna://com.webos.service.capture/executeOneShot').length > 1, 3000, 'frames');
  const log = lunaLog(), grabs = log.filter((c) => c.uri === 'luna://com.webos.service.capture/executeOneShot');
  assert.equal(grabs[0].params.method, 'DISPLAY', 'first, the screen as it was: the glass\'s picture over an app with no video');
  assert.ok(log.indexOf(grabs[0]) < log.map((c) => c.uri).lastIndexOf('luna://com.webos.service.systemUIManager/launch'),
    'taken before the panel is opened over it');
  assert.equal(grabs[1].params.method, 'VIDEO', 'then the video only, not what is drawn over it');
  await until(() => fs.existsSync(path.join(frames, 'frame.png')), 3000, 'the frame file');
  assert.equal(fs.statSync(frames).mode & 0o777, 0o700, 'frames are what is on screen: root only');
  await call('POST', '/api/overlay', { body: { event: 'closed' } });
});

test('overlay: it asks for what it can\'t do itself, and nothing else', async () => {
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: 'network' }, auth: false })).status, 403, 'the token');
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: 'network' } })).status, 200);
  assert.deepEqual(calls('luna://com.webos.applicationManager/launch').at(-1).params,
    { id: 'com.webos.app.firstuse-overlay', params: { target: 'network' } }, "LG's network settings, as its Quick Settings open them");
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: 'settings' } })).status, 200);
  assert.deepEqual(calls('luna://com.webos.applicationManager/launch').at(-1).params, { id: 'com.github.pbalazaar.homeplus' });
  const r = await call('POST', '/api/overlay', { body: { event: 'open', app: 'com.example.anything' } });
  assert.equal(r.status, 500);
  assert.match(r.json.errorText, /not an input/, 'only the inputs it offered');
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: '../../etc' } })).status, 500);
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'whatever' } })).status, 500);
});

test('overlay: once it has closed, the frames stop and are deleted', async () => {
  await call('POST', '/api/key', { body: { key: 'settings' } });
  const frames = () => fs.readdirSync(path.join(dir, 'frames'));
  await until(() => frames().some((f) => fs.existsSync(path.join(dir, 'frames', f, 'frame.png'))), 3000, 'a frame');
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'closed' } })).status, 200);
  await until(() => frames().length === 0, 3000, 'the frames to go');
  const n = calls('luna://com.webos.service.capture/executeOneShot').length;
  await new Promise((done) => setTimeout(done, 800));
  assert.equal(calls('luna://com.webos.service.capture/executeOneShot').length, n, 'no more frames');
});

test('overlay: without its QML (an older package), the Settings key opens LG\'s Quick Settings', async () => {
  const qml = path.join(dir, 'app', 'qml');
  fs.renameSync(qml, qml + '.away');
  try {
    assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'quickSettings' });
    assert.deepEqual(calls('luna://com.webos.service.systemUIManager/launch').at(-1).params, { appId: 'com.webos.app.quicksettings' });
  } finally {
    fs.renameSync(qml + '.away', qml);
  }
});

test('overlay: the Input key opens it on its inputs alone; again, the open panel turns to them, with its frames', async () => {
  assert.deepEqual((await call('POST', '/api/key', { body: { key: 'input' } })).json, { opened: 'overlay' });
  const first = calls('luna://com.webos.service.systemUIManager/launch').at(-1).params;
  assert.equal(first.params.list, 'inputs');
  assert.ok(first.params.state.inputs.length, 'the inputs it offers');
  const frames = path.dirname(fileURLToPath(first.params.frame));
  assert.deepEqual((await call('POST', '/api/key', { body: { key: 'input' } })).json, { opened: 'overlay' });
  const again = calls('luna://com.webos.service.systemUIManager/launch').at(-1).params;
  assert.equal(again.appId, 'com.github.pbalazaar.homeplus.cc');
  assert.deepEqual(again.params, { again: 'input', port, token }, 'asked of the panel on screen: no new state, no new frames');
  assert.ok(fs.existsSync(frames), 'its frames go on');
  await call('POST', '/api/overlay', { body: { event: 'closed' } });
});

test('overlay: when it can\'t open, or fails to load, the Input key gives LG\'s Input picker', async () => {
  await call('POST', '/api/key', { body: { key: 'input' } });
  await call('POST', '/api/overlay', { body: { event: 'failed', error: 'a mistake in the QML' } });
  assert.deepEqual(calls('luna://com.webos.service.systemUIManager/launch').at(-1).params, { appId: 'com.webos.app.quickinputpicker' });
  const qml = path.join(dir, 'app', 'qml');
  fs.renameSync(qml, qml + '.away');
  try {
    assert.deepEqual((await call('POST', '/api/key', { body: { key: 'input' } })).json, { opened: 'inputPicker' });
    assert.deepEqual(calls('luna://com.webos.service.systemUIManager/launch').at(-1).params, { appId: 'com.webos.app.quickinputpicker' });
  } finally {
    fs.renameSync(qml + '.away', qml);
  }
});

test('overlay: with Reduce Transparency (solid panels), no frames of the video are taken', async () => {
  await call('POST', '/api/prefs', { body: { reduceTransparency: true } });
  try {
    const before = calls('luna://com.webos.service.capture/executeOneShot').length;
    await call('POST', '/api/key', { body: { key: 'settings' } });
    const launch = calls('luna://com.webos.service.systemUIManager/launch').at(-1);
    assert.equal(launch.params.params.frame, '', 'no frames to show');
    assert.equal(launch.params.params.state.reduceTransparency, true);
    await new Promise((done) => setTimeout(done, 700));
    assert.equal(calls('luna://com.webos.service.capture/executeOneShot').length, before, 'none taken');
    await call('POST', '/api/overlay', { body: { event: 'closed' } });
  } finally {
    await call('POST', '/api/prefs', { body: { reduceTransparency: false } });
  }
});

// opens it with the stand-in TV's capture giving `what` ({DISPLAY, VIDEO}: see harness.js); its frames'
// folder, once it has taken the screen and asked for the video twice (the first answer dealt with)
async function openWith(what) {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify(what));
  const grabs = () => calls('luna://com.webos.service.capture/executeOneShot').length, before = grabs();
  assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'overlay' });
  await until(() => grabs() >= before + 3, 5000, 'the screen, then the video twice');
  return path.dirname(fileURLToPath(calls('luna://com.webos.service.systemUIManager/launch').at(-1).params.params.frame));
}
// closed, and once its frames' folder has gone (what the TV was taking has come), the stand-in TV's defaults
async function close() {
  await call('POST', '/api/overlay', { body: { event: 'closed' } });
  await until(() => fs.readdirSync(path.join(dir, 'frames')).length === 0, LUNA_MS + 3000, 'the frames to go');
  fs.rmSync(path.join(dir, 'capture.json'), { force: true });
}

test('overlay: video the TV can\'t capture gives the glass no picture (it\'s clear), not a black one', async () => {
  /* PicCap has the TV's one video capture: none of the video, and the screen's is black where it plays */
  try {
    const frames = await openWith({ DISPLAY: 'black', VIDEO: 'busy' });
    assert.deepEqual(fs.readdirSync(frames), [], 'no frame, and nothing left over');
  } finally {
    await close();
  }
});

test('overlay: over an app with no video, the screen as it opened stays the glass\'s picture', async () => {
  try {
    const frames = await openWith({ VIDEO: 'busy' });
    assert.deepEqual(fs.readdirSync(frames), ['frame.png']);
  } finally {
    await close();
  }
});

test('overlay: once the video goes black (or can\'t be captured), its last frame goes', async () => {
  try {
    const frames = await openWith({});
    assert.ok(fs.existsSync(path.join(frames, 'frame.png')), 'a frame of the video');
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'black' }));
    await until(() => !fs.existsSync(path.join(frames, 'frame.png')), 3000, 'the frame to go');
  } finally {
    await close();
  }
});

test('overlay: over video PicCap is capturing, its glass is clear, and PicCap is left alone', async () => {
  assert.equal((await call('GET', '/api/home')).json.piccap, true, 'Settings offers Blur Over Video');
  try {
    const frames = await openWith({ DISPLAY: 'piccap', VIDEO: 'piccap' });
    assert.deepEqual(fs.readdirSync(frames), []);
    assert.equal(calls('luna://org.webosbrew.piccap.service/stop').length, 0);
  } finally {
    await close();
  }
});

test('overlay: Blur Over Video pauses PicCap while it\'s open, for frames of the video, then starts it again', async () => {
  await call('POST', '/api/prefs', { body: { ccBlurVideo: true } });
  try {
    const frames = await openWith({ DISPLAY: 'piccap', VIDEO: 'piccap' });
    assert.equal(calls('luna://org.webosbrew.piccap.service/stop').length, 1, 'paused');
    await until(() => fs.existsSync(path.join(frames, 'frame.png')), 3000, 'a frame of the video');
    const started = calls('luna://org.webosbrew.piccap.service/start').length;
    await close();
    await until(() => calls('luna://org.webosbrew.piccap.service/start').length > started, 3000, 'PicCap to start again');
  } finally {
    await call('POST', '/api/prefs', { body: { ccBlurVideo: false } });
  }
});

test('overlay: the panel asks for the next frame, and the answer comes when it\'s taken', async () => {
  try {
    const frames = await openWith({ VIDEO: 'moving' });
    const first = (await call('POST', '/api/overlay', { body: { event: 'frame', after: 0 } })).json;
    assert.equal(first.picture, true, 'a picture already');
    const asked = Date.now();
    const next = (await call('POST', '/api/overlay', { body: { event: 'frame', after: first.n } })).json;
    assert.ok(next.n > first.n && next.picture, `a newer one: ${JSON.stringify(next)}`);
    assert.ok(Date.now() - asked < 1500, 'as soon as it was taken');
    assert.ok(fs.existsSync(path.join(frames, 'frame.png')));
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'black' }));
    let r = next;
    for (let i = 0; i < 5 && r.picture; i++) r = (await call('POST', '/api/overlay', { body: { event: 'frame', after: r.n } })).json;
    assert.equal(r.picture, false, 'the video went black: no picture (clear glass)');
  } finally {
    await close();
  }
});

// ---- What it asks of the TV's capture: one at a time, so many a second at most, a picture once, less
// often while there's none; and PicCap's pause from one opening to the next.

test('overlay: one capture at a time, across a close and a quick reopen; no snapshot once the panel is asked for', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving', ms: { DISPLAY: 100, VIDEO: GAP - 20 } }));
  const since = Date.now();
  let asked = -1; /* the reopened panel asked for: where, in the luna log */
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    await until(() => videoSince(since).length >= 2 && videoSince(since).at(-1).end === undefined, 3000, 'a frame of the video on its way');
    await call('POST', '/api/overlay', { body: { event: 'closed' } });
    const again = Date.now();
    assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'overlay' });
    asked = lunaLog().map((c) => c.uri).lastIndexOf(LAUNCH);
    await until(() => videoSince(again).filter((c) => c.end).length >= 2, 3000, 'the new one\'s frames');
  } finally {
    await close();
  }
  const all = capturesSince(since);
  assert.equal(new Set(all.map((c) => c.file)).size, all.length, 'each capture has a file of its own');
  oneAtATime(all);
  assert.ok(!lunaLog().slice(asked).some((c) => c.uri === SHOT && c.params.method === 'DISPLAY'),
    'the screen isn\'t taken once the panel has been asked for (it would be of the panel)');
});

test('overlay: so many frames a second at most (three on the TV), each counted from when the last capture started', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving', ms: { VIDEO: 130 } }));
  const since = Date.now();
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    await until(() => videoSince(since).filter((c) => c.end).length >= 6, 5000, 'six frames of the video');
  } finally {
    await close();
  }
  const all = capturesSince(since), gaps = all.slice(1).map((c, i) => c.start - all[i].start).sort((a, b) => a - b);
  assert.ok(gaps[0] >= GAP - 5, `none sooner than ${GAP} ms after the last one started: ${gaps}`);
  assert.ok(gaps[gaps.length >> 1] < GAP + 65, `nor ${GAP} ms after the last one answered (130 ms on): ${gaps}`);
});

test('overlay: a frame the same as the one on show isn\'t sent again (a still screen, a paused video); a new one is', async () => {
  const since = Date.now();
  try {
    const frames = await openWith({}); /* the same picture each time: the screen's, then the video's */
    await until(() => videoSince(since).filter((c) => c.end).length >= 2, 3000, 'two frames of the video');
    const still = await ask(0), file = fs.statSync(path.join(frames, 'frame.png')).ino;
    assert.equal(still.picture, true);
    await until(() => videoSince(since).filter((c) => c.end).length >= 4, 3000, 'two more');
    assert.equal((await ask(0)).n, still.n, 'no new frame: the panel isn\'t woken for the same picture');
    assert.equal(fs.statSync(path.join(frames, 'frame.png')).ino, file, 'nor is the frame written again');
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving' }));
    const next = await ask(still.n);
    assert.ok(next.n === still.n + 1 && next.picture, `a new picture is: ${JSON.stringify(next)}`);
  } finally {
    await close();
  }
});

test('overlay: a capture that comes after it was given up on isn\'t shown, its file goes, and the next waits for it', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving', ms: { VIDEO: GIVE_UP + RETRY[0] + 150 } })); /* (later than the next try would be) */
  const since = Date.now();
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    const frames = framesFolder(), opened = await ask(0);
    assert.equal(opened.picture, true, 'the screen as it opened');
    await until(() => videoSince(since).length >= 2, 4000, 'a frame that came too late, and the next asked for');
    assert.equal((await ask(0)).n, opened.n, 'it isn\'t shown');
    await until(() => fs.readdirSync(frames).join() === 'frame.png', 1000, 'what came too late to be deleted');
    oneAtATime(capturesSince(since)); /* the next was asked for once the one given up on had come */
    // closed while the TV still takes one that was given up on: the folder goes once that has come, and stays gone
    await until(() => Date.now() - videoSince(since).at(-1).start > GIVE_UP + 100, 2000, 'the next one given up on');
    await call('POST', '/api/overlay', { body: { event: 'closed' } });
    await until(() => !fs.existsSync(frames), 3000, 'its folder to go');
    assert.ok(videoSince(since).at(-1).end, 'not before what was being taken into it had come');
    await sleep(300);
    assert.ok(!fs.existsSync(frames), 'nor made again');
  } finally {
    await close();
  }
});

test('overlay: closed while it waits to try again, it stops at once: opened again, its frames come straight away', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'busy' }));
  const since = Date.now();
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    await until(() => videoSince(since).length >= 3, 4000, `three tries (then it waits ${RETRY[2]} ms)`);
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving' }));
    await call('POST', '/api/overlay', { body: { event: 'closed' } });
    const again = Date.now();
    assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'overlay' });
    await until(() => videoSince(again).length >= 1, 3000, 'a frame of the video for the new one');
    const first = videoSince(again)[0].start - again;
    assert.ok(first < GAP + 300, `the new one's first frame came ${first} ms after it opened, not after the last one's wait`);
  } finally {
    await close();
  }
});

test('overlay: a frame that comes after it has closed isn\'t shown, and its folder isn\'t made again', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving', ms: { VIDEO: 300 } }));
  const since = Date.now();
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    const frames = framesFolder();
    await until(() => videoSince(since).length >= 2 && videoSince(since).at(-1).end === undefined, 3000, 'a frame of the video on its way');
    const before = await ask(0);
    await call('POST', '/api/overlay', { body: { event: 'closed' } });
    const closed = Date.now();
    await until(() => !fs.existsSync(frames), 3000, 'its folder to go');
    assert.ok(videoSince(since).at(-1).end >= closed, 'the frame on its way came after it closed');
    assert.deepEqual(await ask(0), { returnValue: true, n: before.n + 1, picture: false }, 'only the close (no picture): the frame isn\'t shown');
    await sleep(400);
    assert.ok(!fs.existsSync(frames), 'nothing made it again');
  } finally {
    await close();
  }
});

test('overlay: a capture the TV never answers holds the next one back only as long as luna waits for it', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'never' }));
  const since = Date.now();
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    await until(() => videoSince(since).length >= 1, 3000, 'a frame of the video asked for');
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'moving' }));
    const n = (await ask(0)).n;
    await until(async () => (await ask(0)).n > n, LUNA_MS + 3000, 'frames again');
    const [hung, next] = videoSince(since);
    assert.ok(next.start - hung.start >= LUNA_MS - 20, `the next one waited for it: ${next.start - hung.start} ms`);
    assert.ok(next.start - hung.start < LUNA_MS + 500, `but no longer: ${next.start - hung.start} ms`);
  } finally {
    await close();
  }
});

test('overlay: while the video can\'t be captured it\'s asked for less and less often, and a picture starts that over', async () => {
  fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'busy' }));
  const since = Date.now(), gaps = (list) => list.slice(1).map((c, i) => c.start - list[i].start);
  try {
    await call('POST', '/api/key', { body: { key: 'settings' } });
    await until(() => videoSince(since).length >= 3, 4000, 'three tries');
    const waits = gaps(videoSince(since));
    for (const [i, ms] of RETRY.slice(0, 2).entries()) assert.ok(waits[i] >= ms - 5 && waits[i] < ms + 200, `waits of ${RETRY}: ${waits}`);
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({})); /* a picture */
    const tries = videoSince(since).length;
    await until(() => videoSince(since).filter((c) => c.end).length > tries, 3000, 'a picture');
    fs.writeFileSync(path.join(dir, 'capture.json'), JSON.stringify({ VIDEO: 'busy' }));
    const again = Date.now();
    await until(() => videoSince(again).length >= 2, 3000, 'two more tries');
    assert.ok(gaps(videoSince(again))[0] < RETRY[1], `the first wait again: ${gaps(videoSince(again))}`);
  } finally {
    await close();
  }
});

test('overlay: Blur Over Video, closed and opened again at once: PicCap stays paused, then starts again once', async () => {
  const piccap = (what) => calls(`luna://org.webosbrew.piccap.service/${what}`).length;
  const stops = piccap('stop'), starts = piccap('start');
  await call('POST', '/api/prefs', { body: { ccBlurVideo: true } });
  try {
    await openWith({ DISPLAY: 'piccap', VIDEO: 'black' }); /* paused, but the video still can't be captured: it waits between tries */
    assert.equal(piccap('stop'), stops + 1, 'paused');
    await call('POST', '/api/overlay', { body: { event: 'closed' } });
    assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'overlay' });
    await sleep(AGAIN_MS + 400); /* (PicCap would start again AGAIN_MS after the first one closed) */
    assert.equal(piccap('start'), starts, 'not started again under the new one');
    assert.equal(piccap('stop'), stops + 1, 'nor stopped again: it stayed paused');
  } finally {
    await close();
    await call('POST', '/api/prefs', { body: { ccBlurVideo: false } });
  }
  await until(() => piccap('start') > starts, 3000, 'PicCap to start again after the last close');
  await sleep(AGAIN_MS + 300);
  assert.equal(piccap('start'), starts + 1, 'once');
});
