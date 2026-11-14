'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs'), os = require('os'), path = require('path');
const { startService, until } = require('./helpers');
const remove = require('../service/remove');
const { mkdirSync } = require('../service/mkdir');
const ROOT = path.join(__dirname, '..');

test('default preview update timer is offline and preserves the supplied foreground app', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-preview-updates-'));
  const probe = path.join(dir, 'probe.cjs'), network = path.join(dir, 'live-fetch'), log = path.join(dir, 'luna.jsonl');
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const app = path.join(dir, 'app');
  mkdirSync(path.join(app, 'qml'));
  for (const file of ['ControlCenter.qml', 'ControlCenterView.qml']) fs.copyFileSync(path.join(ROOT, 'native/qml', file), path.join(app, 'qml', file));
  // Accelerate only the controller timer, and trap its production fetch before it can reach a network.
  // The real harness must supply its own offline boundary, without reusing the foreground-changing UI fixture.
  fs.writeFileSync(probe, `
    const fs = require('fs');
    const release = require(${JSON.stringify(path.join(ROOT, 'service/update-release'))});
    release.fetchRelease = async () => { fs.writeFileSync(${JSON.stringify(network)}, 'attempted'); throw new Error('Unexpected live release fetch'); };
    const updates = require(${JSON.stringify(path.join(ROOT, 'service/updates'))});
    const create = updates.createUpdates;
    updates.createUpdates = opts => create(Object.assign({}, opts, {startupDelay: 1}));
  `);
  let svc;
  try {
    svc = await startService(dir, { NODE_OPTIONS: '--require ' + (probe.includes(' ') ? JSON.stringify(probe) : probe), HOMEPLUS_PREVIEW_UPDATES: '',
      HOMEPLUS_APP_DIR: app, HOMEPLUS_PREVIEW_FOREGROUND: 'netflix', HOMEPLUS_PREVIEW_LUNA_LOG: log,
      HOMEPLUS_INITD_DIR: path.join(dir, 'init.d'), HOMEPLUS_DEFAULT_HOME_DIR: path.join(dir, 'home-state') });
    await until(async () => (await svc.call('GET', '/api/updates')).json.phase === 'error');
    assert.equal(fs.existsSync(network), false, 'default automatic check must not invoke production release fetch');
    const state = (await svc.call('GET', '/api/updates')).json;
    assert.match(state.message, /offline.*preview|preview.*offline/i);
    assert.equal(state.automatic, true, 'isolation must not alter the automatic preference');
    assert.equal(state.currentSource, 'Unknown build', 'isolation must not replace the installed provenance');
    assert.match(fs.readFileSync(path.join(dir, 'service.log'), 'utf8'), /foreground - -> netflix/);
    if (process.platform !== 'win32') {
      assert.deepEqual((await svc.call('POST', '/api/key', {body:{key:'settings'}})).json, {opened:'overlay'});
      const calls = fs.readFileSync(log, 'utf8').trim().split('\n').map(JSON.parse);
      assert(calls.some(c => c.uri === 'luna://com.webos.service.systemUIManager/launch'), 'Settings must still open over the supplied app');
    }
  } finally { if (svc) await svc.stop(); remove.rmSync(dir, {recursive:true,force:true}); }
});
