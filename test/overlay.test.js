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
const net = require('net');
const http = require('http');
const { spawn } = require('child_process');
const { until } = require('./helpers');

const ROOT = path.join(__dirname, '..');
const NODE = process.env.HOMEPLUS_SERVICE_NODE || process.execPath;
let child, port, token, dir, log = '';

const lunaLog = () => (fs.existsSync(path.join(dir, 'luna.log')) ? fs.readFileSync(path.join(dir, 'luna.log'), 'utf8') : '')
  .split('\n').filter(Boolean).map((l) => JSON.parse(l));
const calls = (uri) => lunaLog().filter((c) => c.uri === uri);

function freePort() {
  return new Promise((resolve) => {
    const s = net.createServer().listen(0, '127.0.0.1', () => {
      const p = s.address().port;
      s.close(() => resolve(p));
    });
  });
}

function call(method, p, { body, auth = true } = {}) {
  return new Promise((resolve, reject) => {
    const data = body === undefined ? '' : JSON.stringify(body);
    const h = Object.assign(auth ? { 'X-Homeplus-Token': token } : {}, data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {});
    const req = http.request({ host: '127.0.0.1', port, path: p, method, headers: h }, (res) => {
      let buf = '';
      res.on('data', (c) => { buf += c; });
      res.on('end', () => {
        let json = null;
        try { json = JSON.parse(buf); } catch (e) { /* not JSON */ }
        resolve({ status: res.statusCode, json });
      });
    });
    req.on('error', reject);
    req.end(data || undefined);
  });
}

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-overlay-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix,youtube|YouTube');
  const app = path.join(dir, 'app'); /* the installed app folder, with the Control Center's QML */
  fs.mkdirSync(path.join(app, 'qml'), { recursive: true });
  for (const f of ['ControlCenter.qml', 'ControlCenterView.qml']) fs.copyFileSync(path.join(ROOT, 'native/qml', f), path.join(app, 'qml', f));
  fs.symlinkSync('qml', path.join(app, 'qml-old')); /* an older version's link */
  port = await freePort();
  child = spawn(NODE, [path.join(ROOT, 'tools/native-preview/harness.js'), dir, path.join(ROOT, 'service')], {
    env: Object.assign({}, process.env, {
      NODE_PATH: path.join(ROOT, 'tools/native-preview/stub'),
      HOMEPLUS_CONFIG_DIR: path.join(dir, 'config'),
      HOMEPLUS_TOKEN_FILE: path.join(dir, 'api-token'),
      HOMEPLUS_APP_DIR: app,
      HOMEPLUS_FRAMES_DIR: path.join(dir, 'frames'),
      HOMEPLUS_PREVIEW_LUNA_LOG: path.join(dir, 'luna.log'),
      HOMEPLUS_PORT: String(port),
      HOMEPLUS_PREVIEW: '1',
      HOMEPLUS_PREVIEW_FOREGROUND: 'netflix' /* an app is in front */
    }),
    stdio: ['ignore', 'pipe', 'pipe']
  });
  child.stdout.on('data', (d) => { log += d; });
  child.stderr.on('data', (d) => { log += d; });
  await until(async () => {
    if (child.exitCode !== null) throw new Error(`the service stopped (exit code ${child.exitCode})`);
    try { return (await call('GET', '/api/status', { auth: false })).status === 200; } catch (e) { return false; }
  }, 15000, 'the service to start').catch((e) => { throw new Error(`${e.message}. Its output:\n${log}`); });
  token = fs.readFileSync(path.join(dir, 'api-token'), 'utf8').trim();
});

test.after(async () => {
  if (child && child.exitCode === null) {
    const gone = new Promise((resolve) => child.once('exit', resolve));
    child.kill();
    await gone;
  }
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

test('overlay: the Settings key opens it over the app in front, with the controls, the API and the frames', async () => {
  const r = await call('POST', '/api/key', { body: { key: 'settings' } });
  assert.deepEqual(r.json, { opened: 'overlay' });
  const launch = calls('luna://com.webos.service.systemUIManager/launch').at(-1);
  assert.equal(launch.params.appId, 'com.github.pmiraka.homeplus.cc');
  assert.equal(launch.params.windowType, '_WEBOS_SYSTEM_UI_TYPE_OVERLAY');
  const m = /^file:\/\/(.+)\/(qml-[0-9a-z]+)\/ControlCenter\.qml$/.exec(launch.params.main);
  assert.ok(m, `opened from a link named for the files' time: ${launch.params.main}`);
  assert.equal(fs.readlinkSync(path.join(m[1], m[2])), 'qml');
  assert.ok(!fs.existsSync(path.join(m[1], 'qml-old')), "an older version's link is gone");
  const p = launch.params.params;
  assert.ok(Array.isArray(p.state.controls) && p.state.controls.length, 'the controls on show');
  assert.equal(p.token, token, 'the API token: it can only reach the service over the API');
  assert.equal(p.port, port);
  assert.equal(p.frame, 'file://' + path.join(dir, 'frames', 'frame.jpg'));
  await until(() => calls('luna://com.webos.service.capture/executeOneShot').length > 0, 3000, 'a frame');
  assert.equal(calls('luna://com.webos.service.capture/executeOneShot')[0].params.method, 'VIDEO', 'the video only, not what is drawn over it');
  await until(() => fs.existsSync(path.join(dir, 'frames', 'frame.jpg')), 3000, 'the frame file');
  assert.equal(fs.statSync(path.join(dir, 'frames')).mode & 0o777, 0o700, 'frames are what is on screen: root only');
});

test('overlay: it asks for what it can\'t do itself, and nothing else', async () => {
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: 'network' }, auth: false })).status, 403, 'the token');
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: 'network' } })).status, 200);
  assert.deepEqual(calls('luna://com.webos.applicationManager/launch').at(-1).params,
    { id: 'com.webos.app.firstuse-overlay', params: { target: 'network' } }, "LG's network settings, as its Quick Settings open them");
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: 'settings' } })).status, 200);
  assert.deepEqual(calls('luna://com.webos.applicationManager/launch').at(-1).params, { id: 'com.github.pmiraka.homeplus' });
  const r = await call('POST', '/api/overlay', { body: { event: 'open', app: 'com.example.anything' } });
  assert.equal(r.status, 500);
  assert.match(r.json.errorText, /not an input/, 'only the inputs it offered');
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'open', target: '../../etc' } })).status, 500);
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'whatever' } })).status, 500);
});

test('overlay: once it has closed, the frames stop and are deleted', async () => {
  await call('POST', '/api/key', { body: { key: 'settings' } });
  await until(() => fs.existsSync(path.join(dir, 'frames', 'frame.jpg')), 3000, 'a frame');
  assert.equal((await call('POST', '/api/overlay', { body: { event: 'closed' } })).status, 200);
  await until(() => !fs.existsSync(path.join(dir, 'frames')), 3000, 'the frames to go');
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
