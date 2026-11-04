'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const remove = require('../service/remove');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { startService, sleep, until } = require('./helpers');
const APP = 'com.github.pbalazaar.homeplus';
async function fixture(mode) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-home-api-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const log = path.join(dir, 'luna.log');
  const env = { HOMEPLUS_PREVIEW_DEFAULT_HOME: mode, HOMEPLUS_PREVIEW_LUNA_LOG: log };
  if (mode === 'pending') {
    env.HOMEPLUS_PREVIEW_LUNA_METHOD = 'setHomeOverride';
    env.HOMEPLUS_PREVIEW_LUNA_REQUEST = path.join(dir, 'luna-request');
  }
  const svc = await startService(dir, env);
  return { dir, svc,
    calls: () => fs.readFileSync(log, 'utf8').trim().split('\n').map((x) => JSON.parse(x)),
    async close() {
      try { await svc.call('POST', '/api/home', { body: { defaultHome: false } }); } finally {
        await svc.stop(); remove.rmSync(dir, { recursive: true, force: true });
      }
    }
  };
}
test('actual Home API confirms both settings and keeps explicit LG Home usable', async () => {
  const t = await fixture('supported');
  try {
    let r = await t.svc.call('GET', '/api/home');
    assert.equal(r.status, 200); assert.equal(r.json.defaultHomeAvailable, true);
    assert.equal(r.json.defaultHomeEnabled, false); assert.equal(r.json.startupEnabled, false);
    r = await t.svc.call('POST', '/api/home', { body: { defaultHome: true } });
    assert.equal(r.status, 200, JSON.stringify(r.json)); assert.equal(r.json.mode, 'system');
    assert.equal(r.json.defaultHomeEnabled, true); assert.equal(r.json.noFlashActive, false);
    r = await t.svc.call('POST', '/api/home', { body: { startupEnabled: true } });
    assert.equal(r.json.startupEnabled, true); assert.equal(r.json.defaultHomeEnabled, true);
    const at = Date.now();
    r = await t.svc.call('POST', '/api/launch', { body: { id: 'com.webos.app.home' } });
    assert.equal(r.status, 200);
    await sleep(500);
    assert.equal(t.calls().filter((c) => c.t >= at && c.uri.endsWith('/launch') && c.params.id === APP).length, 0);
    const settings = await t.svc.call('POST', '/api/settings', { body: {} });
    const rows = settings.json.pages.find((x) => x.id === 'remote').rows;
    assert.deepEqual(rows.slice(0, 3).map((x) => x.label), ['Home Button', 'Default Home App', 'Open Home+ at Startup']);
    assert.equal(rows.some((x) => x.id === 'noFlash'), false);
    r = await t.svc.call('POST', '/api/home', { body: { defaultHome: false } });
    assert.equal(r.json.mode, 'off'); assert.equal(r.json.defaultHomeEnabled, false); assert.equal(r.json.startupEnabled, true);
    assert.equal(JSON.parse(fs.readFileSync(path.join(t.dir, 'config/config.json'))).stremioAuthKey, 'preview');
  } finally { await t.close(); }
});
for (const mode of ['unsupported', 'failure']) {
  test('actual Home API leaves a failed ' + mode + ' activation off', async () => {
    const t = await fixture(mode);
    try {
      const r = await t.svc.call('POST', '/api/home', { body: { defaultHome: true } });
      assert.equal(r.status, 500);
      const current = await t.svc.call('GET', '/api/home');
      assert.equal(current.json.defaultHomeEnabled, false);
      assert.equal(current.json.mode, 'watch');
      assert.equal(JSON.parse(fs.readFileSync(path.join(t.dir, 'config/config.json'))).stremioAuthKey, 'preview');
    } finally { await t.close(); }
  });
}
for (const [protocol, mode, payload] of [
  ['HTTP', 'off', { mode: 'off' }], ['HTTP', 'watch', { mode: 'watch' }],
  ['Luna', 'off', { enabled: false }], ['Luna', 'watch', { mode: 'watch' }]
]) {
  test(protocol + ' mode ' + mode + ' rejects a concurrent default Home activation, then works when idle', async () => {
    const t = await fixture('pending');
    const release = path.join(t.dir, 'config/release-default');
    const activating = t.svc.call('POST', '/api/home', { body: { defaultHome: true } });
    const change = async () => {
      if (protocol === 'HTTP') return t.svc.call('POST', '/api/home', { body: payload });
      const file = path.join(t.dir, 'luna-request'), reply = file + '.response';
      try { fs.unlinkSync(reply); } catch (e) { if (e.code !== 'ENOENT') throw e; }
      fs.writeFileSync(file, JSON.stringify(payload));
      await until(() => fs.existsSync(reply), 5000, 'the Luna handler to answer');
      return JSON.parse(fs.readFileSync(reply, 'utf8'));
    };
    try {
      await until(() => fs.existsSync(path.join(t.dir, 'config/default-pending')), 5000, 'the activation to await the bus');
      const denied = await change();
      fs.writeFileSync(release, '');
      const enabled = await activating;
      assert.equal(enabled.status, 200, JSON.stringify(enabled.json));
      assert.equal(enabled.json.defaultHomeEnabled, true);
      if (protocol === 'HTTP') {
        assert.equal(denied.status, 500);
        assert.match(denied.json.errorText, /already in progress/);
      } else {
        assert.equal(denied.returnValue, false);
        assert.match(denied.errorText, /already in progress/);
      }
      const retry = await change();
      assert.equal(protocol === 'HTTP' ? retry.status : retry.returnValue, protocol === 'HTTP' ? 200 : true);
      const state = await t.svc.call('GET', '/api/home');
      assert.equal(state.json.defaultHomeEnabled, false);
      assert.equal(state.json.mode, mode);
    } finally {
      fs.writeFileSync(release, '');
      await activating.catch(() => {});
      await t.close();
    }
  });
}
