'use strict';
const test = require('node:test'), assert = require('node:assert/strict'), fs = require('fs'), path = require('path'), os = require('os');
const { startService, until } = require('./helpers'), remove = require('../service/remove');
test('authenticated production updates HTTP route checks, keeps across restart and rejects forged installs', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-updates-api-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const env = { HOMEPLUS_PREVIEW_UPDATES: 'conflict', HOMEPLUS_PREVIEW_KEEP_CONFIG: '1', HOMEPLUS_INITD_DIR: path.join(dir, 'init.d'), HOMEPLUS_DEFAULT_HOME_DIR: path.join(dir, 'home-state'), HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'keys.json') };
  let svc;
  try {
    svc = await startService(dir, env);
    assert.strictEqual((await svc.call('GET', '/api/updates', { auth: false })).status, 403);
    assert.strictEqual((await svc.call('POST', '/api/updates', { auth: false, body: { action: 'install', version: '99.0.0' } })).status, 403);
    let r = await svc.call('POST', '/api/updates', { body: { action: 'check' } }); assert.strictEqual(r.json.phase, 'checking');
    await until(async () => (await svc.call('GET', '/api/updates')).json.phase !== 'checking');
    r = await svc.call('GET', '/api/updates'); assert.strictEqual(r.json.phase, 'conflict'); assert.strictEqual(r.json.canInstallUpstream, true);
    const forged = await svc.call('POST', '/api/updates', { body: { action: 'install', version: r.json.upstreamVersion, confirmation: 'forged', target: 'upstream', url: 'https://evil.example/payload' } });
    assert.strictEqual(forged.json.returnValue, false);
    await svc.call('POST', '/api/updates', { body: { action: 'keep', version: r.json.upstreamVersion } });
    await svc.stop(); svc = await startService(dir, env);
    r = await svc.call('GET', '/api/updates'); assert.strictEqual(r.json.prompt, false); assert.strictEqual(r.json.ignoredVersion, '99.0.0');
    await svc.call('POST', '/api/updates', { body: { action: 'automatic', enabled: false } });
    r = await svc.call('GET', '/api/updates'); assert.strictEqual(r.json.automatic, false);
    const stateDir = path.join(dir, 'config', 'update-state'); fs.mkdirSync(stateDir);
    fs.writeFileSync(path.join(stateDir, 'transaction.json'), JSON.stringify({ schemaVersion: 1, active: true, phase: 'installing' }));
    const before = fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8');
    assert.strictEqual((await svc.call('POST', '/api/home', { body: { startupEnabled: true } })).json.returnValue, false);
    assert.strictEqual((await svc.call('POST', '/api/uninstall', { body: {} })).json.returnValue, false);
    assert.strictEqual(fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8'), before);

  } finally { if (svc) await svc.stop(); remove.rmSync(dir, { recursive: true, force: true }); }
});


// A real helper and service share the file. Hold the historical detached-write
// window open until the service has committed newer user state.
for (const ordering of ['before', 'after']) test('cancellation preserves service saves ' + ordering + ' Home restoration', async () => {
  const cp = require('child_process'), recovery = require('../service/default-home-guard');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-config-race-'));
  const configDir = path.join(dir, 'config'), stateDir = path.join(configDir, 'updater');
  fs.mkdirSync(configDir); fs.mkdirSync(stateDir); fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  const configPath = path.join(configDir, 'config.json');
  const saved = { homeMode: 'watch', homeOverride: true, onboarded: true, stremioEmail: 'synthetic@example.invalid', layout: { order: ['old'], hidden: [] }, prefs: { sounds: true } };
  recovery.atomic(configPath, Object.assign({}, saved, { homeOverride: false }));
  const s = { schemaVersion: 1, id: 'a'.repeat(24), active: true, phase: 'prepared', previousVersion: require('../service/package.json').version, artifact: { version: '0.26.0' },
    configPath, configBackup: path.join(dir, 'snapshot.json'), directories: [dir, dir, dir], backup: path.join(dir, 'backup.ipk'), targetFile: path.join(dir, 'target.ipk'),
    hook: path.join(dir, 'hook'), boot: path.join(dir, 'boot'), defaultHome: false };
  recovery.atomic(s.configBackup, saved); const file = path.join(stateDir, 'transaction.json'); recovery.atomic(file, s);
  let svc, child, done;
  try {
    svc = await startService(dir, { HOMEPLUS_PREVIEW_KEEP_CONFIG: '1', HOMEPLUS_UPDATE_DIR: stateDir, HOMEPLUS_INITD_DIR: path.join(dir, 'init.d'), HOMEPLUS_DEFAULT_HOME_DIR: path.join(dir, 'home-state'), HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'keys.json') });
    const code = `const fs=require('fs'),cp=require('child_process'),file=process.argv[1],port=process.argv[2],ordering=process.argv[3];
      const recovery=require(${JSON.stringify(require.resolve('../service/default-home-guard'))}),net=require(${JSON.stringify(require.resolve('../service/net'))});
      const state=JSON.parse(fs.readFileSync(file));let held=false;
      function hold(){if(held)return;held=true;fs.writeFileSync(file+'.waiting','');cp.execFileSync(process.execPath,['-e',"const fs=require('fs');const t=setInterval(()=>{if(fs.existsSync(process.argv[1])){clearInterval(t);}},5);",file+'.release'],{timeout:15000});}
      const atomic=recovery.atomic;recovery.atomic=(p,v)=>{if(p===state.configPath&&ordering==='before')hold();return atomic(p,v);};
      const request=net.requestJSON;net.requestJSON=async(method,url,options)=>{if(method==='POST'&&url.endsWith('/api/home')&&ordering==='before')hold();const r=await request(method,url.replace(':47823',':'+port),options);if(method==='POST'&&url.endsWith('/api/home')&&ordering==='after')hold();return r;};
      const worker=require(${JSON.stringify(require.resolve('../service/update-worker'))});
      worker.run(file,Object.assign(worker.production(state),{bootId:()=>'runtime-v2:'+'f'.repeat(64),foreground:async()=>'other.app'})).catch(e=>{console.error(e);process.exitCode=1;});`;
    child = cp.spawn(process.env.HOMEPLUS_SERVICE_NODE || process.execPath, ['-e', code, file, String(svc.port), ordering], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stderr.on('data', d => { output += d; });
    done = new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    await until(() => fs.existsSync(file + '.waiting'), 15000, 'helper restoration boundary');
    assert.strictEqual((await svc.call('POST', '/api/stremio/disconnect', { body: {} })).status, 200);
    assert.strictEqual((await svc.call('POST', '/api/layout', { body: { order: ['new-app'], hidden: ['hidden-app'] } })).status, 200);
    assert.strictEqual((await svc.call('POST', '/api/prefs', { body: { sounds: false, fixture: 'new-value' } })).status, 200);
    fs.writeFileSync(file + '.release', '');
    assert.deepStrictEqual(await done, { code: 0, signal: null }, output);
    const current = JSON.parse(fs.readFileSync(configPath));
    assert.strictEqual(current.stremioEmail, undefined);
    assert.deepStrictEqual(current.layout, { order: ['new-app'], hidden: ['hidden-app'] });
    assert.deepStrictEqual(current.prefs, { sounds: false, fixture: 'new-value' });
    assert.strictEqual(current.homeOverride, true);
    assert.strictEqual(JSON.parse(fs.readFileSync(file)).active, false);
  } finally {
    if (child && child.exitCode === null && child.signalCode === null) { child.kill(); await done; }
    if (svc) await svc.stop(); remove.rmSync(dir, { recursive: true, force: true });
  }
});


test('only the live service restores an unusable config from its validated transaction snapshot', async () => {
  const recovery = require('../service/default-home-guard');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-config-owner-')), stateDir = path.join(dir, 'updater');
  fs.mkdirSync(stateDir); fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  let svc;
  try {
    svc = await startService(dir, { HOMEPLUS_UPDATE_DIR: stateDir, HOMEPLUS_INITD_DIR: path.join(dir, 'init.d'), HOMEPLUS_DEFAULT_HOME_DIR: path.join(dir, 'home-state'), HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'keys.json') });
    const configPath = path.join(dir, 'config', 'config.json'), saved = { accounts: { fixture: 'snapshot' }, layout: { order: ['saved-app'], hidden: [] }, prefs: { sounds: false }, homeOverride: false, homeMode: 'watch' };
    const s = { schemaVersion: 1, id: 'a'.repeat(24), active: true, phase: 'restoring-home', previousVersion: require('../service/package.json').version, artifact: { version: '0.26.0' },
      configPath, configBackup: path.join(dir, 'snapshot.json'), directories: [dir, dir, dir], backup: path.join(dir, 'backup.ipk'), targetFile: path.join(dir, 'target.ipk'), hook: path.join(dir, 'hook'), boot: path.join(dir, 'boot'), defaultHome: false };
    const file = path.join(stateDir, 'transaction.json'); recovery.atomic(s.configBackup, saved); recovery.atomic(file, s);
    const body = { updateTransaction: s.id, updateConfig: 'restore' };
    assert.strictEqual((await svc.call('POST', '/api/home', { auth: false, body })).status, 403);
    const before = fs.readFileSync(configPath, 'utf8');
    for (const bad of [{ updateTransaction: 'b'.repeat(24) }, { updateConfig: 'anything' }]) {
      assert.strictEqual((await svc.call('POST', '/api/home', { body: Object.assign({}, body, bad) })).status, 500);
      assert.strictEqual(fs.readFileSync(configPath, 'utf8'), before);
    }
    for (const invalid of [undefined, '{', 'null', '[]']) {
      if (invalid === undefined) fs.unlinkSync(configPath); else fs.writeFileSync(configPath, invalid);
      const r = await svc.call('POST', '/api/home', { body: Object.assign({}, body, { updateConfig: invalid === undefined ? undefined : 'restore', configPath: '/ignored', config: { accounts: 'forged' } }) });
      assert.strictEqual(r.status, 200); assert.strictEqual(r.json.updateRecovery, 1);
      assert.deepStrictEqual(JSON.parse(fs.readFileSync(configPath)), saved);
    }
    s.configPath = path.join(dir, 'unrelated.json'); recovery.atomic(file, s);
    assert.strictEqual((await svc.call('POST', '/api/home', { body })).status, 500);
    assert.strictEqual(fs.existsSync(s.configPath), false);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(configPath)), saved);
  } finally { if (svc) await svc.stop(); remove.rmSync(dir, { recursive: true, force: true }); }
});
