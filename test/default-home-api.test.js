'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const remove = require('../service/remove');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { startService, sleep } = require('./helpers');
const APP = 'com.github.pbalazaar.homeplus';
async function fixture(mode) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-home-api-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const log = path.join(dir, 'luna.log');
  const svc = await startService(dir, { HOMEPLUS_PREVIEW_DEFAULT_HOME: mode, HOMEPLUS_PREVIEW_LUNA_LOG: log });
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
