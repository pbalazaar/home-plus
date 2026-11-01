'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const remove = require('../service/remove');
const fs = require('fs'), os = require('os'), path = require('path');
const { spawn } = require('child_process');
const { until, sleep } = require('./helpers');
const recovery = require('../service/default-home-guard');
const APP = 'com.github.pbalazaar.homeplus', LG = 'com.webos.app.home';
function fixture(deadline) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-independent-')), app = path.join(dir, 'app');
  fs.mkdirSync(app);
  fs.writeFileSync(path.join(app, 'appinfo.json'), JSON.stringify({ id: APP, appCategory: ['home'] }));
  fs.writeFileSync(path.join(app, 'homeplus'), 'native');
  const helper = path.join(dir, 'guard.js'), binary = path.join(dir, "luna's fixture"), file = path.join(dir, 'state.json');
  fs.copyFileSync(path.join(__dirname, '../service/default-home-guard.js'), helper);
  fs.copyFileSync(path.join(__dirname, 'fixtures/system-home-luna.js'), binary); fs.chmodSync(binary, 0o755);
  const config = path.join(dir, 'config.json'), model = path.join(dir, 'model.json');
  recovery.atomic(config, { defaultHomeEnabled: true, homeOverride: true, homeMode: 'watch', account: 'keep' });
  recovery.atomic(model, { defaults: { home: APP, browser: 'changed.browser' }, calls: [] });
  recovery.atomic(file, { version: 1, active: true, deadline, transaction: 'owned', applicationDir: app,
    configPath: config, stockHome: LG, before: { browser: 'old.browser' }, previous: { homeMode: 'watch', homeOverride: false } });
  const children = [], detached = [];
  const read = (f) => JSON.parse(fs.readFileSync(f, 'utf8'));
  function start(mode) {
    const child = spawn(process.env.HOMEPLUS_SERVICE_NODE || process.execPath, [helper, mode, file], {
      env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: binary, HOMEPLUS_MOCK_LUNA_STATE: model }),
      stdio: ['ignore', 'pipe', 'pipe']
    });
    children.push(child); let output = '';
    child.stdout.on('data', (x) => { output += x; }); child.stderr.on('data', (x) => { output += x; });
    const done = new Promise((resolve, reject) => {
      child.on('error', reject); child.on('exit', (code) => code === 0 ? resolve(output) : reject(new Error(output)));
    });
    return { child, done };
  }
  return { dir, app, file, config, model, read, start, detached,
    async close() {
      const s = read(file); s.active = false; recovery.atomic(file, s);
      for (const c of children) if (c.exitCode === null && c.signalCode === null) {
        const exited = new Promise((resolve) => c.once('exit', resolve)); c.kill('SIGTERM'); await exited;
      }
      for (const pid of detached) { try { process.kill(pid, 'SIGTERM'); } catch (e) { if (e.code !== 'ESRCH') throw e; } }
      remove.rmSync(dir, { recursive: true, force: true });
    }
  };
}
test('independent boot recovery rejects an unfinished trial immediately', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(Date.now() + 120000);
  try {
    const output = await t.start('--start').done; t.detached.push(JSON.parse(output).pid);
    await until(() => !t.read(t.file).active, 7000);
    assert.deepEqual(t.read(t.model).defaults, { browser: 'changed.browser' });
    assert.deepEqual(t.read(t.config), { homeMode: 'watch', homeOverride: false, account: 'keep' });
  } finally { await t.close(); }
});
test('permanent recovery survives its starter and handles removal outside the app', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(null);
  try {
    const output = await t.start('--start').done; t.detached.push(JSON.parse(output).pid);
    await until(() => fs.existsSync(t.file + '.ready'));
    await sleep(300); assert.equal(t.read(t.file).active, true);
    fs.unlinkSync(path.join(t.app, 'homeplus'));
    await until(() => !t.read(t.file).active, 10000);
    assert.deepEqual(t.read(t.model).defaults, { browser: 'changed.browser' });
    assert.equal(t.read(t.config).homeOverride, false);
    assert.equal(t.read(t.config).account, 'keep');
  } finally { await t.close(); }
});
test('a brief package update is tolerated, but a downgraded non-Home manifest is restored', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(null);
  try {
    const g = t.start('--watch');
    await until(() => fs.existsSync(t.file + '.ready'));
    fs.unlinkSync(path.join(t.app, 'homeplus'));
    await sleep(300); fs.writeFileSync(path.join(t.app, 'homeplus'), 'updated native');
    await sleep(2200); assert.equal(t.read(t.file).active, true);
    recovery.atomic(path.join(t.app, 'appinfo.json'), { id: APP });
    await until(() => !t.read(t.file).active, 10000); await g.done;
    assert.deepEqual(t.read(t.model).defaults, { browser: 'changed.browser' });
    assert.equal(t.read(t.config).account, 'keep');
  } finally { await t.close(); }
});

test('a replaced transaction retires its sleeping watcher without touching the new default', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(null);
  try {
    const g = t.start('--watch');
    await until(() => fs.existsSync(t.file + '.ready'));
    const s = t.read(t.file); s.transaction = 'replacement'; recovery.atomic(t.file, s);
    await g.done;
    assert.equal(t.read(t.file).active, true);
    assert.equal(t.read(t.model).defaults.home, APP);
    assert.equal(t.read(t.model).calls.length, 0);
  } finally { await t.close(); }
});
test('replacement during a delayed recovery health check cannot change config or defaults', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(Date.now() - 1);
  try {
    const m = t.read(t.model); m.healthDelay = 750; recovery.atomic(t.model, m);
    const g = t.start('--watch');
    await until(() => t.read(t.model).calls.length);
    const s = t.read(t.file); s.transaction = 'replacement'; recovery.atomic(t.file, s);
    await g.done;
    assert.equal(t.read(t.file).active, true);
    assert.equal(t.read(t.config).defaultHomeEnabled, true);
    assert.equal(t.read(t.model).defaults.home, APP);
    assert.equal(t.read(t.model).calls.length, 1);
  } finally { await t.close(); }
});
