/* A web LG Home with fake mount commands: exercise the real HTTP and Luna handlers without mounting. */
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { startService, until } = require('./helpers');

test('failed unmount is reported to HTTP and Luna, remains visible, and retries the same target', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-exit-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const fail = path.join(dir, 'fail-umount');
  const calls = path.join(dir, 'mount-calls');
  const request = path.join(dir, 'luna-request');
  const svc = await startService(dir, { HOMEPLUS_PREVIEW_REDIRECT: '1', HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'hook', 'keybinds.json'), HOMEPLUS_PREVIEW_LUNA_METHOD: 'setHomeOverride', HOMEPLUS_PREVIEW_LUNA_REQUEST: request });
  const lunaCall = async (payload) => {
    const reply = request + '.response';
    try { fs.unlinkSync(reply); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    fs.writeFileSync(request, JSON.stringify(payload));
    await until(() => fs.existsSync(reply), 5000, 'Luna reply');
    return JSON.parse(fs.readFileSync(reply, 'utf8'));
  };
  try {
    const enabled = await svc.call('POST', '/api/home', { body: { mode: 'redirect' } });
    assert.equal(enabled.status, 200, JSON.stringify(enabled.json));
    assert.equal(enabled.json.noFlashActive, true);
    fs.writeFileSync(fail, 'fail');
    const http = await svc.call('POST', '/api/home', { body: { mode: 'off' } });
    assert.equal(http.status, 500);
    assert.match(http.json.errorText, /could not unmount.*simulated unmount failure/);
    const state = (await svc.call('GET', '/api/home')).json;
    assert.equal(state.mode, 'redirect', 'the launcher can offer disable again');
    assert.equal(state.noFlashActive, true);
    const luna = await lunaCall({ enabled: false });
    assert.equal(luna.returnValue, false);
    assert.match(luna.errorText, /could not unmount/);
    const attempts = () => fs.readFileSync(calls, 'utf8').trim().split('\n').map(JSON.parse).filter((c) => c.cmd === 'umount');
    assert.equal(attempts().length, 2);
    assert.deepEqual(attempts()[0].args, attempts()[1].args, 'failed target is retained for retry');
    fs.unlinkSync(fail);
    const retry = await lunaCall({ enabled: false });
    assert.equal(retry.returnValue, true);
    assert.equal(retry.mode, 'off');
    assert.deepEqual(attempts()[2].args, attempts()[0].args);
    const after = (await svc.call('GET', '/api/home')).json;
    assert.equal(after.mode, 'off');
    assert.equal(after.noFlashActive, false);
  } finally {
    await svc.stop();
    fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
  }
});

test('disabled Home binding stays removed across service restart', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-restart-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const hook = path.join(dir, 'hook'); fs.mkdirSync(hook);
  const binds = path.join(hook, 'keybinds.json');
  fs.writeFileSync(binds, JSON.stringify({ 773: { action: 'launch', id: 'com.github.pbalazaar.homeplus' }, 42: { action: 'launch', id: 'other' } }));
  const env = { HOMEPLUS_INPUTHOOK_BINDS: binds, HOMEPLUS_PREVIEW_KEEP_CONFIG: '1' };
  let svc = await startService(dir, env);
  try {
    assert.equal((await svc.call('POST', '/api/home', { body: { mode: 'off' } })).status, 200);
    await svc.stop(); svc = await startService(dir, env);
    assert.equal((await svc.call('GET', '/api/home')).json.mode, 'off');
    assert.deepEqual(JSON.parse(fs.readFileSync(binds, 'utf8')), { 42: { action: 'launch', id: 'other' } });
  } finally { await svc.stop(); fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 }); }
});
