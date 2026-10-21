/* Ambient light: HyperHDR's LEDs and PicCap's screen capture as Control Center switches,
 * against a stand-in HyperHDR (its JSON-RPC) and a fake PicCap service. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { mockServer, quiet, freshRequire, until } = require('./helpers');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-ambient-'));
test.after(() => fs.rmSync(dir, { recursive: true, force: true }));
const PAUSED = path.join(dir, 'piccap-paused'); /* the mark that PicCap's capture is paused */
process.env.HOMEPLUS_PICCAP_PAUSED = PAUSED;

/* HyperHDR's JSON-RPC: serverinfo with its components, and componentstate turning one on or off */
async function fakeHyperhdr(leds) {
  const state = { leds };
  const server = await mockServer((req, url, body) => {
    const cmd = JSON.parse(body || '{}');
    if (url.pathname !== '/json-rpc') return null;
    if (cmd.command === 'serverinfo') {
      return { json: { command: 'serverinfo', success: true, tan: cmd.tan, info: { components: [
        { name: 'ALL', enabled: true }, { name: 'LEDDEVICE', enabled: state.leds }, { name: 'VIDEOGRABBER', enabled: true }] } } };
    }
    if (cmd.command === 'componentstate' && cmd.componentstate.component === 'LEDDEVICE') {
      state.leds = cmd.componentstate.state;
      return { json: { command: 'componentstate', success: true, tan: cmd.tan } };
    }
    return { json: { success: false, error: 'unknown command' } };
  });
  return Object.assign(server, { state });
}

/* the two switches, with HyperHDR at `url` (none: not running) and PicCap's service answering `piccap` */
function ambient({ url = 'http://127.0.0.1:9/json-rpc', hyperhdr = true, piccap = true, capturing = false } = {}) {
  for (const [name, on] of [['hyperhdr', hyperhdr], ['piccap', piccap]]) {
    const d = path.join(dir, name);
    if (on) fs.mkdirSync(d, { recursive: true });
    else fs.rmSync(d, { recursive: true, force: true });
  }
  process.env.HOMEPLUS_HYPERHDR = url;
  process.env.HOMEPLUS_HYPERHDR_DIR = path.join(dir, 'hyperhdr');
  process.env.HOMEPLUS_PICCAP_DIR = path.join(dir, 'piccap');
  const calls = [], pic = { running: capturing };
  const luna = async (uri, params) => {
    calls.push({ uri, params });
    if (uri === 'luna://org.webosbrew.piccap.service/status') return { returnValue: true, isRunning: pic.running, videoRunning: pic.running };
    if (uri === 'luna://org.webosbrew.piccap.service/start') { pic.running = true; return { returnValue: true }; }
    if (uri === 'luna://org.webosbrew.piccap.service/stop') { pic.running = false; return { returnValue: true }; }
    if (uri === 'luna://org.webosbrew.hyperhdr.loader.service/start') return { returnValue: true };
    return { returnValue: false, errorText: 'no such service' };
  };
  const [lights, capture] = freshRequire('../service/ambient')({ luna, log: quiet });
  return { lights, capture, calls, pic };
}

test('ambient: offered only when HyperHDR and PicCap are installed', async () => {
  const both = ambient();
  assert.equal(await both.lights.available(), true);
  assert.equal(await both.capture.available(), true);
  const none = ambient({ hyperhdr: false, piccap: false });
  assert.equal(await none.lights.available(), false);
  assert.equal(await none.capture.available(), false);
});

test('ambient: Ambient Light is HyperHDR\'s LEDs, on and off', async () => {
  const hdr = await fakeHyperhdr(false);
  try {
    const { lights } = ambient({ url: `${hdr.base}/json-rpc` });
    assert.deepEqual(await lights.read(), { on: false, value: 'Off' });
    await lights.run();
    assert.equal(hdr.state.leds, true);
    assert.deepEqual(await lights.read(), { on: true, value: 'On' });
    await lights.run();
    assert.equal(hdr.state.leds, false);
  } finally {
    await hdr.close();
  }
});

test('ambient: with HyperHDR not running, Ambient Light is off, and OK starts it', async () => {
  const { lights, calls } = ambient(); /* nothing answers on port 9 */
  assert.deepEqual(await lights.read(), { on: false, value: 'Off' });
  await lights.run();
  assert.ok(calls.some((c) => c.uri === 'luna://org.webosbrew.hyperhdr.loader.service/start'));
});

test('ambient: Screen Capture is PicCap\'s, started and stopped through its own service', async () => {
  const { capture, pic } = ambient({ capturing: true });
  assert.deepEqual(await capture.read(), { on: true, value: 'On' });
  await capture.run();
  assert.equal(pic.running, false);
  await capture.run();
  assert.equal(pic.running, true);
});

test('ambient: Blur Over Video pauses PicCap\'s capture (its switch still says On), and starts it again after', async () => {
  const { capture, pic } = ambient({ capturing: true });
  assert.equal(await capture.pause(), true);
  assert.equal(pic.running, false);
  assert.ok(fs.existsSync(PAUSED), 'marked, in case the service stops');
  assert.deepEqual(await capture.read(), { on: true, value: 'On' }, 'paused, not switched off');
  assert.equal(await capture.pause(), false, 'once');
  await capture.resume();
  assert.equal(pic.running, true);
  assert.equal(fs.existsSync(PAUSED), false);
});

test('ambient: nothing to pause when PicCap isn\'t capturing, or isn\'t there', async () => {
  const idle = ambient({ capturing: false });
  assert.equal(await idle.capture.pause(), false);
  const gone = ambient({ piccap: false, capturing: true });
  assert.equal(await gone.capture.pause(), false);
  for (const a of [idle, gone]) assert.ok(!a.calls.some((c) => c.uri.endsWith('/stop')), 'PicCap left alone');
});

test('ambient: Screen Capture switched off while it\'s paused stays off', async () => {
  const { capture, pic } = ambient({ capturing: true });
  await capture.pause();
  await capture.run();
  assert.deepEqual(await capture.read(), { on: false, value: 'Off' });
  await capture.resume();
  assert.equal(pic.running, false);
  assert.equal(fs.existsSync(PAUSED), false);
});

test('ambient: PicCap paused by a service that stopped is started again by the next', async () => {
  fs.writeFileSync(PAUSED, '');
  const { pic } = ambient({ capturing: false });
  await until(() => pic.running, 3000, 'PicCap to be started');
  assert.equal(fs.existsSync(PAUSED), false);
});
