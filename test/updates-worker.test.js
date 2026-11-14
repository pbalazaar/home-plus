'use strict';
const test = require('node:test'), assert = require('node:assert/strict'), fs = require('fs'), path = require('path'), os = require('os');
const archive = require('../service/update-archive'), worker = require('../service/update-worker'), recovery = require('../service/default-home-guard');
const { mkdirSync } = require('../service/mkdir'), remove = require('../service/remove');
const APP = 'com.github.pbalazaar.homeplus', SERVICE = APP + '.service';
async function assertKilled(child, file) {
  const result = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolve({ code, signal }));
  });
  assert.deepStrictEqual(result, process.platform === 'win32' ? { code: 1, signal: null } : { code: null, signal: 'SIGKILL' });
  assert.strictEqual(fs.existsSync(file + '.graceful-exit'), false);
}
function files(version) {
  const f = {};
  function add(i, name, value) { f[archive.roots[i] + '/' + name] = { mode: name === 'homeplus' ? 0o755 : 0o644, data: Buffer.from(typeof value === 'object' ? JSON.stringify(value) : value) }; }
  add(0, 'appinfo.json', { id: APP, version, type: 'native', main: 'homeplus' }); add(0, 'homeplus', 'native-' + version);
  add(1, 'package.json', { name: SERVICE, version }); add(1, 'services.json', { id: SERVICE }); add(1, 'service.js', 'service-' + version);
  add(2, 'packageinfo.json', { id: APP, app: APP, version, services: [SERVICE] }); return f;
}
function put(entries, dirs) {
  Object.keys(entries).forEach(name => { const i = archive.roots.findIndex(r => name.indexOf(r + '/') === 0), file = path.join(dirs[i], name.slice(archive.roots[i].length + 1)); mkdirSync(path.dirname(file)); fs.writeFileSync(file, entries[name].data, { mode: entries[name].mode }); });
}
async function fixture(failure) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-worker-')), dirs = ['app', 'service', 'package'].map(n => path.join(dir, n));
  const before = files('0.25.0'), after = files('0.26.0'); put(before, dirs);
  const old = archive.pack(before, '0.25.0'), target = archive.pack(after, '0.26.0');
  const config = { accounts: { fixture: 'private-sentinel' }, layout: ['app1'], inputKey: true, startupEnabled: false, prefs: { theme: 'dark' } };
  const file = path.join(dir, 'transaction.json');
  const s = { schemaVersion: 1, id: 'a'.repeat(24), active: true, phase: 'prepared', previousVersion: '0.25.0', artifact: { version: '0.26.0', size: target.length, sha256: archive.hash(target) }, directories: dirs,
    backup: path.join(dir, 'previous.ipk'), targetFile: path.join(dir, 'target.ipk'), configPath: path.join(dir, 'config.json'), configBackup: path.join(dir, 'previous-config.json'),
    hook: path.join(dir, '01-homeplus-update'), boot: path.join(dir, 'boot.sh'), defaultHome: false, backupHash: archive.hash(old), targetInventory: (await archive.inspect(target, '0.26.0')).inventory };
  fs.writeFileSync(s.backup, old); fs.writeFileSync(s.targetFile, target); recovery.atomic(s.configPath, config); recovery.atomic(s.configBackup, config); recovery.atomic(file, s); fs.writeFileSync(s.boot, 'fixture'); fs.symlinkSync(s.boot, s.hook);
  let targetInstalls = 0, backupInstalls = 0, fallback = false;
  const ops = { bootId: fixtureBoot, foreground: async () => failure === 'left' ? 'other.app' : APP,
    async installTarget() { targetInstalls++; if (failure === 'install') throw new Error('install failed'); put(after, dirs); },
    async installBackup() { backupInstalls++; put(before, dirs); },
    async elevate() { if (failure === 'elevate' && !backupInstalls) throw new Error('elevation failed'); },
    async health(v) { assert.strictEqual(JSON.parse(fs.readFileSync(path.join(dirs[0], 'appinfo.json'))).version, v); if (failure === 'health' && !backupInstalls) throw new Error('health failed'); },
    async enter() { return true; }, async restoreHome() { require('../service/updates').restoreConfig(s, cfg => recovery.atomic(s.configPath, cfg)); }, async fallback() { fallback = true; } };
  return { dir, dirs, file, s, before, after, ops, config, counts: () => ({ targetInstalls, backupInstalls, fallback }), close() { remove.rmSync(dir, { recursive: true, force: true }); } };
}
test('archive rejects wrong identity, paths and damaged headers; backup is reinstallable', async () => {
  const f = files('0.25.0'), ipk = archive.pack(f, '0.25.0');
  assert.strictEqual(Object.keys((await archive.inspect(ipk, '0.25.0')).inventory).length, 6);
  await assert.rejects(archive.inspect(ipk, '0.26.0'), /identity/);
  f['etc/shadow'] = { mode: 0o600, data: Buffer.from('bad') };
  await assert.rejects(archive.inspect(archive.pack(f, '0.25.0'), '0.25.0'), /outside/);
  delete f['etc/shadow']; f['../escape'] = { mode: 0o644, data: Buffer.from('bad') }; assert.throws(() => archive.pack(f, '0.25.0'), /Unsafe/);
  const corrupt = Buffer.from(ipk); corrupt[0] = 0; await assert.rejects(archive.inspect(corrupt, '0.25.0'), /header/);
});
test('progress acknowledgement is not terminal success', () => {
  assert.strictEqual(worker.finalReply({ returnValue: true, statusText: 'Installing' }, false), false);
  assert.strictEqual(worker.finalReply({ returnValue: true, finished: true }, false), true);
  assert.strictEqual(worker.finalReply({ statusValue: 30, details: { packageId: APP } }, true), true);
  assert.strictEqual(!!worker.finalReply({ statusValue: 30, details: { packageId: 'other.app' } }, true), false);
  assert.throws(() => worker.finalReply({ returnValue: false, finished: true }, false));
});
test('verified replacement preserves accounts, layout, startup and input', async () => {
  const f = await fixture();
  try { await worker.run(f.file, f.ops); const s = JSON.parse(fs.readFileSync(f.file)); assert.strictEqual(s.phase, 'completed'); assert.strictEqual(s.active, false); assert.strictEqual(fs.existsSync(f.s.hook), false); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config); assert.strictEqual(f.counts().backupInstalls, 0); }
  finally { f.close(); }
});
for (const failure of ['install', 'elevate', 'health']) test(failure + ' failure reinstalls previous package and preserves private state', async () => {
  const f = await fixture(failure);
  try { await worker.run(f.file, f.ops); const s = JSON.parse(fs.readFileSync(f.file)); assert.strictEqual(s.phase, 'failed'); assert.strictEqual(s.active, false); assert.strictEqual(f.counts().backupInstalls, 1); assert.strictEqual(f.counts().fallback, true); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config); assert.strictEqual(JSON.parse(fs.readFileSync(path.join(f.dirs[0], 'appinfo.json'))).version, '0.25.0'); }
  finally { f.close(); }
});
test('leaving Home cancels before replacement', async () => {
  const f = await fixture('left'); try { await worker.run(f.file, f.ops); assert.deepStrictEqual(f.counts(), { targetInstalls: 0, backupInstalls: 0, fallback: false }); assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, false); } finally { f.close(); }
});
test('interrupted replacement recovers without repeating target install', async () => {
  const f = await fixture(); f.s.phase = 'installing'; f.s.installBoot = 'runtime-v2:' + 'e'.repeat(64); recovery.atomic(f.file, f.s); put(f.after, f.dirs);
  try { await worker.run(f.file, f.ops); assert.strictEqual(f.counts().targetInstalls, 0); assert.strictEqual(f.counts().backupInstalls, 1); assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'failed'); } finally { f.close(); }
});
test('copied helper completes after replacement kills its launching process', async () => {
  const f = await fixture(), childProcess = require('child_process');
  try {
    for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js'])
      fs.copyFileSync(path.join(__dirname, '..', 'service', name), path.join(f.dir, name));
    const runner = path.join(f.dir, 'fixture-runner.js');
    fs.writeFileSync(runner, "'use strict';\n" +
      "const fs=require('fs'),path=require('path'),a=require('./update-archive'),w=require('./update-worker'),m=require('./mkdir');\n" +
      "const file=process.argv[2],s=w.load(file);\n" +
      "const replace=async p=>{const r=await a.inspect(fs.readFileSync(p),p===s.backup?s.previousVersion:s.artifact.version);Object.keys(r.files).forEach(n=>{if(r.files[n].directory)return;const i=a.roots.findIndex(x=>n.indexOf(x+'/')===0),f=path.join(s.directories[i],n.slice(a.roots[i].length+1));m.mkdirSync(path.dirname(f));fs.writeFileSync(f,r.files[n].data);});};\n" +
      "w.run(file,{bootId:()=>'runtime-v2:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff',foreground:async()=>'" + APP + "',installTarget:async()=>{await replace(s.targetFile);process.kill(Number(process.argv[3]),'SIGKILL');},installBackup:()=>replace(s.backup),elevate:async()=>{},health:async()=>{},enter:async()=>true,restoreHome:async()=>{},fallback:async()=>{}}).then(()=>console.log('helper-completed'),e=>{console.error(e.message);process.exitCode=1;});\n");
    const parent = childProcess.spawn(process.execPath, ['-e',
      "process.on('exit',()=>require('fs').writeFileSync(process.argv[2]+'.graceful-exit','unexpected'));" +
      "const c=require('child_process').spawn(process.execPath,[process.argv[1],process.argv[2],String(process.pid)],{detached:true,stdio:['ignore',1,2]});c.unref();setInterval(()=>{},1000);",
      runner, f.file], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '', errors = '';
    parent.stdout.on('data', data => { output += data; });
    parent.stderr.on('data', data => { errors += data; });
    // The detached helper inherits these pipes. 'close' waits for it to finish too,
    // including worker lock cleanup after transaction.active becomes false.
    await assertKilled(parent, f.file);
    assert.strictEqual(errors, '');
    assert.strictEqual(output.trim(), 'helper-completed');
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, false);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'completed');
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config);
  } finally { f.close(); }
});
test('installer subscription uses a real PTY and waits for finished, not early acknowledgement', { skip: process.platform !== 'linux' }, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-installer-')), previous = process.env.PATH;
  try {
    const tool = path.join(dir, 'luna-send');
    fs.writeFileSync(tool, '#!/bin/sh\nprintf \'{"returnValue":true,"statusText":"Installing"}\\n\'\nsleep 0.1\nprintf \'{"returnValue":true,"finished":true}\\n\'\nsleep 5\n', { mode: 0o700 });
    process.env.PATH = dir + ':' + previous;
    const start = Date.now();
    await worker.installSubscription('luna://fixture/install', {}, false, { timeout: 3000 });
    assert(Date.now() - start >= 80);
    fs.writeFileSync(tool, '#!/bin/sh\nprintf \'{"returnValue":true,"statusText":"Installing"}\\n\'\nsleep 5\n', { mode: 0o700 });
    await assert.rejects(worker.installSubscription('luna://fixture/install', {}, false, { timeout: 100 }), /final completion/);
  } finally { process.env.PATH = previous; remove.rmSync(dir, { recursive: true, force: true }); }
});
test('unknown installer completion never races HBC with a rollback', async () => {
  const f = await fixture();
  f.ops.installTarget = async () => { const e = new Error('completion unknown'); e.installerUncertain = true; throw e; };
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'uncertain');
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, true);
    assert.strictEqual(f.counts().backupInstalls, 0);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    const s = JSON.parse(fs.readFileSync(f.file)); s.installBoot = 'runtime-v2:' + 'e'.repeat(64); recovery.atomic(f.file, s);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 1);
  } finally { f.close(); }
});
test('insufficient storage and unsupported prerequisites refuse before package replacement', async () => {
  assert.throws(() => worker.checkSpace('Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/mock 100 90 10 90% /var', 1000, 1000), /free space/);
  assert.throws(() => worker.checkSpace('unreadable', 1000, 1000), /free space/);
  worker.checkSpace('Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/mock 100000 0 100000 0% /var', 1000, 1000);
  const f = await fixture();
  try {
    await assert.rejects(worker.prepare({ isRoot: () => false, stateDir: f.dir }), /elevated webOS/);
    assert.strictEqual(JSON.parse(fs.readFileSync(path.join(f.dirs[0], 'appinfo.json'))).version, '0.25.0');
  } finally { f.close(); }
});
test('tampered staged target aborts without replacing either package', async () => {
  const f = await fixture(); fs.appendFileSync(f.s.targetFile, 'tamper');
  try { await worker.run(f.file, f.ops); assert.deepStrictEqual(f.counts(), { targetInstalls: 0, backupInstalls: 0, fallback: false }); assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'error'); }
  finally { f.close(); }
});
test('a second independent helper refuses the live worker lock', { skip: process.platform !== 'linux' }, async () => {
  const f = await fixture(), cp = require('child_process'), until = require('./helpers').until;
  let child;
  try {
    for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js'])
      fs.copyFileSync(path.join(__dirname, '..', 'service', name), path.join(f.dir, name));
    const luna = path.join(f.dir, 'fixture-luna');
    fs.writeFileSync(luna, '#!/bin/sh\nsleep 2\n', { mode: 0o700 });
    const opts = { env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: luna }), stdio: ['ignore', 'ignore', 'pipe'] };
    const runner = path.join(f.dir, 'live-worker.js');
    fs.writeFileSync(runner, "const w=require('./update-worker'),file=process.argv[2];w.run(file,Object.assign(w.production(w.load(file)),{bootId:()=>'" + fixtureBoot() + "'})).catch(e=>{console.error(e.message);process.exitCode=1;});");
    const args = [runner, f.file];
    child = cp.spawn(process.execPath, args, opts);
    await until(() => fs.existsSync(f.file + '.worker'), 3000);
    const second = cp.spawn(process.execPath, args, opts); let output = '';
    second.stderr.on('data', data => { output += data; });
    assert.strictEqual(await new Promise(resolve => second.on('exit', resolve)), 1);
    assert.match(output, /already running/);
    assert.strictEqual(JSON.parse(fs.readFileSync(path.join(f.dirs[0], 'appinfo.json'))).version, '0.25.0');
  } finally {
    if (child && child.exitCode === null) { const exited = new Promise(resolve => child.on('exit', resolve)); child.kill(); await exited; }
    f.close();
  }
});

// Review regressions: later user state must outlive an update transaction's snapshot.
function laterConfig(f) {
  return Object.assign({}, f.config, { accounts: { fixture: 'refreshed-token' }, layout: ['new-app'], inputKey: false, startupEnabled: true,
    prefs: { theme: 'light', sounds: false }, updates: { automatic: false, ignoredVersion: '0.26.0' }, addedAfterSnapshot: true });
}
const fixtureBoot = () => 'runtime-v2:' + 'f'.repeat(64);
for (const failure of ['left', 'tampered']) test('pre-replacement ' + failure + ' preserves post-snapshot user settings', async () => {
  const f = await fixture(failure), current = laterConfig(f);
  recovery.atomic(f.s.configPath, current);
  if (failure === 'tampered') fs.appendFileSync(f.s.targetFile, 'tampered');
  try { await worker.run(f.file, f.ops); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current); assert.strictEqual(f.counts().backupInstalls, 0); }
  finally { f.close(); }
});
test('legitimate config writes during installation do not cause rollback', async () => {
  const f = await fixture(), install = f.ops.installTarget, current = laterConfig(f);
  f.ops.installTarget = async () => { await install(); recovery.atomic(f.s.configPath, current); };
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'completed');
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current);
  } finally { f.close(); }
});
test('delayed recovery preserves newer account, layout, preference and update fields', async () => {
  const f = await fixture(), current = laterConfig(f);
  f.s.phase = 'uncertain'; f.s.installBoot = 'runtime-v2:' + 'e'.repeat(64); recovery.atomic(f.file, f.s); recovery.atomic(f.s.configPath, current); put(f.after, f.dirs);
  const install = f.ops.installBackup;
  f.ops.installBackup = async () => { await install(); current.duringRecovery = 'retained'; recovery.atomic(f.s.configPath, current); };
  try { await worker.run(f.file, f.ops); assert.strictEqual(f.counts().backupInstalls, 1); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current); }
  finally { f.close(); }
});
test('uncertain recovery install records the current boot before invoking the installer', async () => {
  const f = await fixture(); f.s.phase = 'uncertain'; f.s.installBoot = 'runtime-v2:' + 'e'.repeat(64); recovery.atomic(f.file, f.s);
  let backupCalls = 0, observed;
  f.ops.installBackup = async () => {
    backupCalls++; observed = JSON.parse(fs.readFileSync(f.file));
    const e = new Error('backup completion unknown'); e.installerUncertain = true; throw e;
  };
  try {
    await assert.rejects(worker.run(f.file, f.ops), /could not complete/);
    assert.strictEqual(observed.installBoot, fixtureBoot());
    assert.strictEqual(observed.installerPending, true);
    await worker.run(f.file, f.ops);
    assert.strictEqual(backupCalls, 1);
  } finally { f.close(); }
});
for (const kind of ['target', 'backup']) test('helper SIGKILL during ' + kind + ' install cannot start a same-boot rollback', async () => {
  const f = await fixture(), cp = require('child_process'), runner = path.join(f.dir, 'interrupted-helper.js');
  for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js'])
    fs.copyFileSync(path.join(__dirname, '..', 'service', name), path.join(f.dir, name));
  if (kind === 'backup') { f.s.phase = 'uncertain'; f.s.installBoot = 'runtime-v2:' + 'e'.repeat(64); recovery.atomic(f.file, f.s); }
  fs.writeFileSync(runner, "process.on('exit',()=>require('fs').writeFileSync(process.argv[2]+'.graceful-exit','unexpected'));" +
    "const w=require('./update-worker');w.run(process.argv[2],{bootId:()=>'runtime-v2:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff',foreground:async()=>'" + APP +
    "',fallback:async()=>{}," + (kind === 'target' ? 'installTarget' : 'installBackup') + ":async()=>process.kill(process.pid,'SIGKILL')}).catch(()=>process.exit(2));");
  try {
    const child = cp.spawn(process.execPath, [runner, f.file], { stdio: 'ignore' });
    await assertKilled(child, f.file);
    const interrupted = JSON.parse(fs.readFileSync(f.file));
    assert.strictEqual(interrupted.phase, kind === 'target' ? 'installing' : 'recovering');
    assert.strictEqual(interrupted.installerPending, true); assert.strictEqual(interrupted.installBoot, fixtureBoot());
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'uncertain');
    const s = JSON.parse(fs.readFileSync(f.file)); s.installBoot = 'runtime-v2:' + 'e'.repeat(64); recovery.atomic(f.file, s);
    await worker.run(f.file, f.ops); assert.strictEqual(f.counts().backupInstalls, 1);
  } finally { f.close(); }
});
test('cancellation restores only updater-owned Home fields in usable current config', async () => {
  const f = await fixture('left');
  Object.assign(f.config, { homeMode: 'redirect', homeOverride: true, defaultHomeEnabled: false });
  recovery.atomic(f.s.configBackup, f.config);
  const current = Object.assign(laterConfig(f), { homeOverride: false, defaultHomeEnabled: true });
  recovery.atomic(f.s.configPath, current);
  try {
    await worker.run(f.file, f.ops);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), Object.assign({}, current, { homeOverride: true }));
  } finally { f.close(); }
});
test('complete config snapshot is used only when current config is absent or unusable', async () => {
  for (const invalid of [undefined, '{', 'null', '[]']) {
    const f = await fixture('left');
    if (invalid === undefined) fs.unlinkSync(f.s.configPath); else fs.writeFileSync(f.s.configPath, invalid);
    try { await worker.run(f.file, f.ops); assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), f.config); }
    finally { f.close(); }
  }
});

test('leaving Home during archive validation cancels at the replacement boundary', async () => {
  const f = await fixture(), inspect = archive.inspect;
  let front = APP;
  f.ops.foreground = async () => front;
  archive.inspect = async function () { const result = await inspect.apply(this, arguments); front = 'user.selected.app'; return result; };
  try {
    await worker.run(f.file, f.ops);
    assert.deepStrictEqual(f.counts(), { targetInstalls: 0, backupInstalls: 0, fallback: false });
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, false);
    assert.match(JSON.parse(fs.readFileSync(f.file)).message, /cancelled/);
  } finally { archive.inspect = inspect; f.close(); }
});
test('an accepted native launch that immediately exits is unhealthy, not deferred', async () => {
  const luna = recovery.luna; let launches = 0;
  recovery.luna = async uri => {
    if (uri.endsWith('/launch')) { launches++; return { returnValue: true }; }
    return { returnValue: true, appId: launches ? 'com.webos.app.home' : APP };
  };
  try { await assert.rejects(worker.production({}).enter(), /did not stay open/); assert.strictEqual(launches, 1); }
  finally { recovery.luna = luna; }
});
test('native entry defers to an already selected foreground app without launching', async () => {
  const luna = recovery.luna; let launches = 0;
  recovery.luna = async uri => {
    if (uri.endsWith('/launch')) launches++;
    return { returnValue: true, appId: 'user.selected.app' };
  };
  try { assert.strictEqual(await worker.production({}).enter(), false); assert.strictEqual(launches, 0); }
  finally { recovery.luna = luna; }
});
for (const stale of [false, true]) for (const point of ['writing', 'published']) test('SIGKILL while ' + point + (stale ? ' a stale lock successor' : ' the worker lock') + ' permits a safe restart', async () => {
  const f = await fixture(), cp = require('child_process'), runner = path.join(f.dir, 'lock-interruption.js');
  if (stale) recovery.atomic(f.file + '.worker', { pid: 2147483647, boot: fixtureBoot(), token: 'dead-owner' });
  fs.writeFileSync(runner, "const fs=require('fs'),file=process.argv[2],point=process.argv[3],write=fs.writeFileSync,link=fs.linkSync;" +
    "process.on('exit',()=>write(file+'.graceful-exit','unexpected'));" +
    "fs.writeFileSync=function(p){if(typeof p==='number'||p===file+'.worker'){" +
    "if(point==='writing'){const fd=typeof p==='number'?p:fs.openSync(p,'wx',384);fs.writeSync(fd,'{');process.kill(process.pid,'SIGKILL');}" +
    "const result=write.apply(this,arguments);if(p===file+'.worker')process.kill(process.pid,'SIGKILL');return result;}return write.apply(this,arguments);};" +
    "fs.linkSync=function(a,b){const result=link.apply(this,arguments);if(b===file+'.worker'||b.indexOf(file+'.worker.after-')===0)process.kill(process.pid,'SIGKILL');return result;};" +
    "require(" + JSON.stringify(require.resolve('../service/update-worker')) + ").run(file,{bootId:()=>'runtime-v2:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff'}).catch(()=>process.exit(2));");
  try {
    const child = cp.spawn(process.execPath, [runner, f.file, point], { stdio: 'ignore' });
    await assertKilled(child, f.file);
    if (fs.existsSync(f.file + '.worker')) assert.strictEqual(typeof JSON.parse(fs.readFileSync(f.file + '.worker')).pid, 'number');
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().targetInstalls, 1);
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'completed');
  } finally { f.close(); }
});

test('healthy service with a crashed native entry reinstalls the previous package', async () => {
  const f = await fixture(), luna = recovery.luna, healthy = [], health = f.ops.health;
  let launched = false;
  recovery.luna = async uri => {
    if (uri.endsWith('/launch')) { launched = true; return { returnValue: true }; }
    return { returnValue: true, appId: launched ? 'com.webos.app.home' : APP };
  };
  const enter = worker.production(f.s).enter;
  f.ops.health = async version => { await health(version); healthy.push(version); };
  f.ops.enter = async () => f.counts().backupInstalls ? true : enter();
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().targetInstalls, 1);
    assert.strictEqual(f.counts().backupInstalls, 1);
    assert(healthy.includes('0.26.0')); assert(healthy.includes('0.25.0'));
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).phase, 'failed');
  } finally { recovery.luna = luna; f.close(); }
});
test('concurrent stale-lock takeover grants ownership to only one live helper', async () => {
  const f = await fixture(), cp = require('child_process'), until = require('./helpers').until, children = [];
  f.s.phase = 'uncertain'; f.s.installBoot = fixtureBoot(); recovery.atomic(f.file, f.s);
  recovery.atomic(f.file + '.worker', { pid: 2147483647, boot: fixtureBoot(), token: 'dead-owner' });
  const runner = path.join(f.dir, 'lock-contender.js');
  fs.writeFileSync(runner, "const fs=require('fs'),file=process.argv[2],id=process.argv[3];fs.writeFileSync(file+'.ready-'+id,'ready');" +
    "const timer=setInterval(()=>{if(!fs.existsSync(file+'.go'))return;clearInterval(timer);require(" +
    JSON.stringify(require.resolve('../service/update-worker')) + ").run(file,{bootId:()=>'runtime-v2:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff',fallback:async()=>{" +
    "fs.writeFileSync(file+'.inside',String(process.pid),{flag:'wx'});await new Promise(r=>setTimeout(r,500));fs.unlinkSync(file+'.inside');" +
    "}}).then(()=>process.exit(0),e=>process.exit(/already running/.test(e.message)?3:4));},10);");
  try {
    const exits = [];
    for (let i = 0; i < 4; i++) {
      const child = cp.spawn(process.execPath, [runner, f.file, String(i)], { stdio: 'ignore' }); children.push(child);
      exits.push(new Promise(resolve => child.on('exit', resolve)));
    }
    await until(() => children.every((child, i) => fs.existsSync(f.file + '.ready-' + i)), 5000);
    fs.writeFileSync(f.file + '.go', 'go');
    const codes = await Promise.all(exits);
    assert.strictEqual(codes.filter(code => code === 0).length, 1);
    assert.strictEqual(codes.filter(code => code === 3).length, 3);
    assert.strictEqual(fs.existsSync(f.file + '.worker'), false);
  } finally {
    for (const child of children) if (child.exitCode === null) child.kill();
    f.close();
  }
});
test('an unverifiable pre-existing worker lock is never removed blindly', async () => {
  const f = await fixture();
  fs.writeFileSync(f.file + '.worker', '{');
  try {
    await assert.rejects(worker.run(f.file, f.ops), SyntaxError);
    assert.strictEqual(fs.readFileSync(f.file + '.worker', 'utf8'), '{');
    assert.strictEqual(f.counts().targetInstalls, 0);
    assert.strictEqual(f.counts().backupInstalls, 0);
  } finally { f.close(); }
});

test('application PulseAudio cookie is private runtime data, including legacy backup inventories', async () => {
  const f = await fixture(), relative = '.config/pulse/cookie', key = archive.roots[0] + '/' + relative;
  const cookie = path.join(f.dirs[0], relative), fake = Buffer.alloc(256, 7);
  mkdirSync(path.dirname(cookie)); fs.writeFileSync(cookie, fake); fs.chmodSync(cookie, 0o777);
  const legacy = await archive.inspect(archive.pack(Object.assign({}, f.before, { [key]: { mode: 0o777, data: fake } }), '0.25.0'), '0.25.0');
  const read = fs.readFileSync;
  try {
    fs.readFileSync = function (file) { if (file === cookie) throw new Error('Runtime cookie contents must not be read'); return read.apply(this, arguments); };
    const snapshot = archive.snapshot(f.dirs);
    assert.strictEqual(snapshot[key], undefined);
    assert.deepStrictEqual(Object.keys(snapshot).sort(), Object.keys(f.before).sort());
    const clean = await archive.inspect(archive.pack(snapshot, '0.25.0'), '0.25.0');
    assert.strictEqual(clean.files[key], undefined);
    archive.verifyInstalled(clean.inventory, f.dirs);
    fs.writeFileSync(cookie, Buffer.alloc(256, 9));
    archive.verifyInstalled(legacy.inventory, f.dirs);
    fs.unlinkSync(cookie);
    archive.verifyInstalled(legacy.inventory, f.dirs);
    fs.writeFileSync(path.join(f.dirs[0], 'homeplus'), 'changed payload');
    assert.throws(() => archive.verifyInstalled(legacy.inventory, f.dirs), /verification/);
  } finally { fs.readFileSync = read; f.close(); }
});
test('cookie exception never hides neighbouring code, service paths or non-regular impostors', async () => {
  const f = await fixture(), expected = (await archive.inspect(archive.pack(f.before, '0.25.0'), '0.25.0')).inventory;
  const cookie = path.join(f.dirs[0], '.config/pulse/cookie');
  try {
    for (const file of [path.join(f.dirs[0], '.config/pulse/extra.js'), path.join(f.dirs[0], '.config/extra.js'), path.join(f.dirs[1], '.config/pulse/cookie')]) {
      mkdirSync(path.dirname(file)); fs.writeFileSync(file, 'unexpected code');
      assert.throws(() => archive.verifyInstalled(expected, f.dirs), /Unexpected/);
      fs.unlinkSync(file);
    }
    fs.symlinkSync(path.join(f.dirs[0], 'homeplus'), cookie);
    assert.throws(() => archive.snapshot(f.dirs), /Unsupported/);
    assert.throws(() => archive.verifyInstalled(expected, f.dirs), /Unsupported/);
    fs.unlinkSync(cookie); fs.mkdirSync(cookie);
    assert.throws(() => archive.snapshot(f.dirs), /Unsupported/);
    fs.rmdirSync(cookie);
    if (process.platform === 'linux') {
      require('child_process').execFileSync('mkfifo', [cookie]);
      assert.throws(() => archive.snapshot(f.dirs), /Unsupported/);
      fs.unlinkSync(cookie);
    }
  } finally { f.close(); }
});

// Volatile boot-generation fixtures never use the host's real /dev.
function bootFixture() {
  const dir = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-boot-'))), runtime = path.join(dir, 'dev');
  fs.mkdirSync(runtime); fs.chmodSync(runtime, 0o755);
  const mount = '24 21 0:21 / /dev/shm rw - tmpfs tmpfs rw\n26 20 0:23 / ' + runtime + ' rw - tmpfs tmpfs rw\n40 20 0:35 / /tmp rw - tmpfs tmpfs rw\n';
  const markerDir = path.join(runtime, 'homeplus-update-boot'), marker = path.join(markerDir, 'generation.json');
  function owned(fn, alter) {
    const original = fs.lstatSync;
    fs.lstatSync = function (file) {
      const st = original.apply(this, arguments);
      if (file === runtime || file.indexOf(runtime + path.sep) === 0) {
        st.uid = 0; // represent the TV's root-owned mount, even on an unprivileged desktop
        if (process.platform === 'win32') st.mode = (st.mode & ~0o777) | (file === runtime ? 0o755 : st.isDirectory() ? 0o700 : 0o600);
        if (alter) alter(file, st);
      }
      return st;
    };
    try { return fn(); } finally { fs.lstatSync = original; }
  }
  return { dir, runtime, mount, markerDir, marker, owned,
    read: () => owned(() => worker.bootId(runtime, mount)),
    close() { remove.rmSync(dir, { recursive: true, force: true }); } };
}
test('volatile boot generation survives repeat reads and changes only after runtime reset', () => {
  const f = bootFixture();
  try {
    const first = f.read();
    assert.match(first, /^runtime-v2:[0-9a-f]{64}$/);
    assert.strictEqual(f.read(), first);
    assert.strictEqual(require('../service/update-worker').bootId, worker.bootId);
    assert.strictEqual(f.read(), first); // another caller/service start sees the same file
    fs.unlinkSync(f.marker);
    assert.throws(() => f.read()); assert.throws(() => f.read());
    assert.strictEqual(fs.existsSync(f.marker), false);
    remove.rmSync(f.markerDir, { recursive: true }); // fixture models complete tmpfs reset on reboot
    assert.notEqual(f.read(), first);
  } finally { f.close(); }
});
test('existing missing, torn and invalid generations never regenerate in the same runtime', () => {
  for (const value of [null, '{', '{"schemaVersion":1,"generation":"bad"}']) {
    const f = bootFixture();
    try {
      fs.mkdirSync(f.markerDir, 0o700);
      if (value !== null) fs.writeFileSync(f.marker, value, { mode: 0o600 });
      assert.throws(() => f.read());
      assert.throws(() => f.read());
      assert.strictEqual(fs.existsSync(f.marker), value !== null);
      if (value !== null) assert.strictEqual(fs.readFileSync(f.marker, 'utf8'), value);
    } finally { f.close(); }
  }
});
test('boot generation refuses unverified mounts, permissions and marker symlinks', () => {
  const f = bootFixture();
  try {
    assert.throws(() => f.owned(() => worker.bootId(f.runtime, f.mount.replace(f.runtime + ' rw - tmpfs ', f.runtime + ' rw - ext4 '))));
    assert.throws(() => f.owned(() => worker.bootId(f.runtime, '' )));
    assert.throws(() => f.owned(() => worker.bootId(f.runtime, f.mount), (file, st) => { if (file === f.runtime) st.uid = 1; }));
    assert.throws(() => f.owned(() => worker.bootId(f.runtime, f.mount), (file, st) => { if (file === f.runtime) st.mode |= 0o002; }));
    assert.strictEqual(fs.existsSync(f.markerDir), false);
    f.read(); fs.unlinkSync(f.marker);
    const target = path.join(f.dir, 'other'); fs.writeFileSync(target, '{"schemaVersion":1,"generation":"' + 'a'.repeat(64) + '"}');
    fs.symlinkSync(target, f.marker);
    assert.throws(() => f.read());
  } finally { f.close(); }
});
for (const legacy of [true, false]) test((legacy ? 'legacy UUID' : 'current generation') + ' installer stays quarantined until a later generation', async () => {
  const f = await fixture(), first = 'runtime-v2:' + 'a'.repeat(64), second = 'runtime-v2:' + 'b'.repeat(64);
  let current = first;
  f.ops.bootId = () => current;
  f.s.phase = 'uncertain'; f.s.installerPending = true;
  f.s.installBoot = legacy ? '2a69c7c5-0dc5-45cd-80d4-7426f0ef3a29' : first;
  recovery.atomic(f.file, f.s);
  if (legacy) recovery.atomic(f.file + '.worker', { pid: 2147483647, boot: f.s.installBoot });
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).installBoot, first);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    current = second;
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 1);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).installBoot, second);
  } finally { f.close(); }
});
test('legacy live worker ownership is not invalidated by boot-token migration', async () => {
  const f = await fixture();
  recovery.atomic(f.file + '.worker', { pid: process.pid, boot: '2a69c7c5-0dc5-45cd-80d4-7426f0ef3a29' });
  f.ops.bootId = () => 'runtime-v2:' + 'a'.repeat(64);
  try {
    await assert.rejects(worker.run(f.file, f.ops), /already running/);
    assert.strictEqual(f.counts().targetInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file + '.worker')).pid, process.pid);
  } finally { f.close(); }
});
test('unavailable boot identity starts no installer and preserves the active recovery record', async () => {
  const f = await fixture();
  f.s.phase = 'uncertain'; f.s.installerPending = true; recovery.atomic(f.file, f.s);
  f.ops.bootId = () => { throw new Error('Boot generation unavailable'); };
  try {
    await assert.rejects(worker.run(f.file, f.ops), /Boot generation/);
    assert.strictEqual(f.counts().targetInstalls, 0); assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).active, true);
    assert.strictEqual(fs.existsSync(f.s.hook), true);
  } finally { f.close(); }
});

function bootChild(f, body) {
  const source = "const fs=require('fs'),path=require('path'),runtime=" + JSON.stringify(f.runtime) + ",mount=" + JSON.stringify(f.mount) +
    ",stat=fs.lstatSync;fs.lstatSync=function(file){const s=stat.apply(this,arguments);if(file===runtime||file.indexOf(runtime+path.sep)===0){s.uid=0;" +
    "if(process.platform==='win32')s.mode=(s.mode&~511)|(file===runtime?493:s.isDirectory()?448:384);}return s;};" +
    "const w=require(" + JSON.stringify(require.resolve('../service/update-worker')) + ");" + body;
  return require('child_process').spawn(process.execPath, ['-e', source], { stdio: ['ignore', 'pipe', 'pipe'] });
}
test('concurrent boot-marker creators and readers converge without replacing a generation', async () => {
  const f = bootFixture();
  try {
    const children = [];
    for (let i = 0; i < 4; i++) {
      const child = bootChild(f, "try{console.log(JSON.stringify({id:w.bootId(runtime,mount)}));}catch(e){console.log(JSON.stringify({error:e.code}));}");
      children.push(new Promise((resolve, reject) => {
        let out = '', err = ''; child.stdout.on('data', data => { out += data; }); child.stderr.on('data', data => { err += data; });
        child.once('error', reject); child.once('close', code => resolve({ code, out, err }));
      }));
    }
    const results = await Promise.all(children), generation = f.read();
    let successes = 0;
    for (const result of results) {
      assert.strictEqual(result.code, 0); assert.strictEqual(result.err, '');
      const value = JSON.parse(result.out);
      if (value.id) { successes++; assert.strictEqual(value.id, generation); }
      else assert.strictEqual(value.error, 'ENOENT'); // concurrent first publication may safely refuse
    }
    assert(successes > 0);
    assert.strictEqual(f.read(), generation);
  } finally { f.close(); }
});
for (const point of ['writing', 'published']) test('killed boot-marker creator while ' + point + ' cannot silently regenerate', async () => {
  const f = bootFixture(), base = path.join(f.dir, 'creator');
  const guard = JSON.stringify(require.resolve('../service/default-home-guard'));
  const child = bootChild(f, "process.on('exit',()=>fs.writeFileSync(" + JSON.stringify(base + '.graceful-exit') + ",'unexpected'));" +
    "const r=require(" + guard + "),atomic=r.atomic;r.atomic=function(file,value){if(" + JSON.stringify(point) +
    "==='writing'){fs.writeFileSync(file+'.partial','{');process.kill(process.pid,'SIGKILL');}atomic(file,value);process.kill(process.pid,'SIGKILL');};w.bootId(runtime,mount);");
  child.stdout.resume(); child.stderr.resume();
  try {
    await assertKilled(child, base);
    assert.strictEqual(fs.existsSync(f.markerDir), true);
    if (point === 'writing') {
      assert.strictEqual(fs.existsSync(f.marker), false);
      assert.throws(() => f.read()); assert.throws(() => f.read());
      assert.strictEqual(fs.existsSync(f.marker), false);
    } else {
      const previous = JSON.parse(fs.readFileSync(f.marker));
      assert.strictEqual(f.read(), 'runtime-v2:' + previous.generation);
    }
  } finally { f.close(); }
});

// Runtime parent portability: the production location is fixed, never a fallback list.
for (const type of ['tmpfs', 'devtmpfs']) test('boot generation accepts protected [SECRET_xiuzfr]-owned ' + type + ' parents only', () => {
  const f = bootFixture(), mount = f.mount.replace(f.runtime + ' rw - tmpfs ', f.runtime + ' rw - ' + type + ' ');
  const metadata = (mode, uid) => (file, st) => { if (file === f.runtime) { st.mode = (st.mode & ~0o7777) | mode; st.uid = uid; } };
  try {
    for (const mode of [0o777, 0o1777]) assert.throws(() => f.owned(() => worker.bootId(f.runtime, mount), metadata(mode, 0)));
    assert.throws(() => f.owned(() => worker.bootId(f.runtime, mount), metadata(0o755, 5944)));
    assert.strictEqual(fs.existsSync(f.markerDir), false);
    const generation = f.owned(() => worker.bootId(f.runtime, mount), metadata(0o755, 0));
    assert.match(generation, /^runtime-v2:[0-9a-f]{64}$/);
    assert.strictEqual(f.owned(() => worker.bootId(f.runtime, mount), metadata(0o755, 0)), generation);
  } finally { f.close(); }
});
test('production boot identity uses fixed /dev and never falls through to another mount', () => {
  const f = bootFixture(), read = fs.readFileSync, lstat = fs.lstatSync, seen = [];
  const directory = lstat(f.runtime);
  fs.readFileSync = function (file) {
    if (file === '/proc/self/mountinfo') return '21 20 0:6 / /dev rw - devtmpfs devtmpfs rw\n26 20 0:23 / /run rw - tmpfs tmpfs rw\n40 20 0:35 / /tmp rw - tmpfs tmpfs rw\n';
    return read.apply(this, arguments);
  };
  fs.lstatSync = function (file) { seen.push(file); directory.uid = 5944; return directory; };
  try {
    assert.throws(() => worker.bootId(), /runtime mount/);
    assert.deepStrictEqual(seen, ['/dev']);
  } finally { fs.readFileSync = read; fs.lstatSync = lstat; f.close(); }
});
test('runtime-v1 pending installer is baselined to v2 without treating relocation as reboot', async () => {
  const f = await fixture();
  let current = 'runtime-v2:' + 'a'.repeat(64);
  f.ops.bootId = () => current;
  f.s.phase = 'uncertain'; f.s.installerPending = true; f.s.installBoot = 'runtime-v1:' + 'b'.repeat(64);
  recovery.atomic(f.file, f.s);
  try {
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file)).installBoot, current);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 0);
    current = 'runtime-v2:' + 'c'.repeat(64);
    await worker.run(f.file, f.ops);
    assert.strictEqual(f.counts().backupInstalls, 1);
  } finally { f.close(); }
});
test('runtime-v1 live worker ownership survives migration to the /dev v2 family', async () => {
  const f = await fixture();
  f.ops.bootId = () => 'runtime-v2:' + 'a'.repeat(64);
  recovery.atomic(f.file + '.worker', { pid: process.pid, boot: 'runtime-v1:' + 'b'.repeat(64) });
  try {
    await assert.rejects(worker.run(f.file, f.ops), /already running/);
    assert.strictEqual(f.counts().targetInstalls, 0);
    assert.strictEqual(JSON.parse(fs.readFileSync(f.file + '.worker')).pid, process.pid);
  } finally { f.close(); }
});


test('unavailable service retains cancelled transaction and hook without an offline config write', async () => {
  const f = await fixture('left'), cp = require('child_process'), current = laterConfig(f);
  const originalRestore = f.ops.restoreHome;
  f.ops.restoreHome = async () => {
    // Another real process is still saving even though the Home API is unavailable.
    cp.execFileSync(process.execPath, ['-e', "const fs=require('fs');const p=process.argv[1];fs.writeFileSync(p+'.new',process.argv[2]);fs.renameSync(p+'.new',p);", f.s.configPath, JSON.stringify(current)]);
    throw new Error('service unavailable');
  };
  try {
    await worker.run(f.file, f.ops);
    let state = worker.load(f.file);
    assert.strictEqual(state.active, true); assert.strictEqual(state.cancelled, true); assert.strictEqual(state.phase, 'restoring-home');
    assert.strictEqual(fs.existsSync(f.s.hook), true); assert.strictEqual(f.counts().fallback, true);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current);
    // When its owner returns, cancellation finishes without installing either package.
    f.ops.restoreHome = originalRestore; await worker.run(f.file, f.ops); state = worker.load(f.file);
    assert.strictEqual(state.active, false); assert.strictEqual(state.phase, 'error');
    assert.strictEqual(f.counts().targetInstalls, 0); assert.strictEqual(f.counts().backupInstalls, 0);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current);
  } finally { f.close(); }
});

test('independent LG Home fallback never writes private settings while another process saves', async t => {
  const f = await fixture(), cp = require('child_process'), current = laterConfig(f);
  f.s.homeStateFile = path.join(f.dir, 'home-state.json');
  recovery.atomic(f.s.homeStateFile, { version: 1, active: true, deadline: null, stockHome: 'com.webos.app.lghome',
    before: { home: 'com.webos.app.lghome' }, previous: { homeOverride: false }, applicationDir: f.dirs[0], configPath: f.s.configPath });
  let restored = false;
  t.mock.method(recovery, 'restore', async () => {
    cp.execFileSync(process.execPath, ['-e', "const fs=require('fs');const p=process.argv[1];fs.writeFileSync(p+'.new',process.argv[2]);fs.renameSync(p+'.new',p);", f.s.configPath, JSON.stringify(current)]);
    restored = true;
  });
  const atomic = recovery.atomic;
  t.mock.method(recovery, 'atomic', (file, value) => { assert.notEqual(file, f.s.configPath, 'detached fallback cannot own config'); return atomic(file, value); });
  try {
    await worker.production(f.s).fallback();
    assert.strictEqual(restored, true);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), current);
    assert.strictEqual(worker.load(f.file).active, true);
    assert.strictEqual(fs.existsSync(f.s.hook), true);
    assert.strictEqual(recovery.load(f.s.homeStateFile).active, false);
  } finally { f.close(); }
});

// Run production restoration in a separate process, with only its loopback
// destination changed to a bounded stand-in for the official 0.25 service.
for (const reply of ['accepted', 'ignored', 'invalid-config']) test('official Home API fallback: ' + reply, async () => {
  const f = await fixture(), cp = require('child_process'), { mockServer } = require('./helpers');
  const current = laterConfig(f); recovery.atomic(f.s.configPath, current);
  if (reply === 'invalid-config') fs.writeFileSync(f.s.configPath, '{');
  fs.writeFileSync(path.join(f.dirs[0], 'api-token'), 'fixture-token');
  let calls = 0;
  const server = await mockServer((req, url, body) => {
    if (url.pathname === '/api/status') return { json: { id: SERVICE, version: '0.25.0' } };
    assert.strictEqual(req.headers['x-homeplus-token'], 'fixture-token');
    const action = JSON.parse(body); assert.deepStrictEqual(action, { mode: 'watch' }); calls++;
    if (reply === 'ignored') return { json: { returnValue: true } };
    const cfg = JSON.parse(fs.readFileSync(f.s.configPath)); cfg.homeMode = action.mode; cfg.homeOverride = true; recovery.atomic(f.s.configPath, cfg);
    return { json: { mode: action.mode } };
  });
  try {
    const code = "const net=require(" + JSON.stringify(require.resolve('../service/net')) + "),request=net.requestJSON;net.requestJSON=(m,u,o)=>request(m,u.replace('http://127.0.0.1:47823',process.argv[2]),o);" +
      "const w=require(" + JSON.stringify(require.resolve('../service/update-worker')) + ");w.production(w.load(process.argv[1])).restoreHome().then(()=>console.log('restored'),()=>{process.exitCode=2;});";
    const child = cp.spawn(process.execPath, ['-e', code, f.file, server.base], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', data => { output += data; });
    const result = await new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    assert.strictEqual(result.signal, null); assert.strictEqual(result.code, reply === 'accepted' ? 0 : 2);
    assert.strictEqual(calls, reply === 'invalid-config' ? 0 : 1);
    if (reply === 'accepted') {
      assert.strictEqual(output.trim(), 'restored');
      assert.deepStrictEqual(JSON.parse(fs.readFileSync(f.s.configPath)), Object.assign({}, current, { homeMode: 'watch', homeOverride: true }));
    } else assert.strictEqual(fs.readFileSync(f.s.configPath, 'utf8'), reply === 'invalid-config' ? '{' : JSON.stringify(current, null, 2));
  } finally { await server.close(); f.close(); }
});


test('post-install elevation replaces the real stale service process before exact-version health', { skip: process.platform !== 'linux' }, async () => {
  const cp = require('child_process'), dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-service-restart-'));
  const runner = path.join(dir, 'restart.js'), version = path.join(dir, 'version');
  fs.writeFileSync(version, '0.25.0');
  const code = `const fs=require('fs'),cp=require('child_process'),assert=require('assert'),file=process.argv[2],SERVICE=${JSON.stringify(SERVICE)};
    const realExec=cp.execFile;let elevations=0,svc,decoy;
    cp.execFile=(command,args,options,done)=>{if(command.endsWith('/elevate-service')){assert.deepStrictEqual(args,[SERVICE]);elevations++;return done(null,'');}return realExec(command,args,options,done);};
    const net=require(${JSON.stringify(require.resolve('../service/net'))}),request=net.requestJSON;
    net.requestJSON=(method,url,options)=>request(method,url.replace(':47823',':'+svc.port),options);
    const guard=require(${JSON.stringify(require.resolve('../service/default-home-guard'))});
    function start(title,ignore){return new Promise((resolve,reject)=>{
      const source="process.title=process.argv[1];const version=require('fs').readFileSync(process.argv[2],'utf8');if(process.argv[3]==='ignore')process.on('SIGTERM',()=>{});const server=require('http').createServer((q,r)=>r.end(JSON.stringify({id:process.argv[4],version,elevated:true})));server.listen(0,'127.0.0.1',()=>console.log(server.address().port));";
      const child=cp.spawn(process.execPath,['-e',source,title,file,ignore?'ignore':'normal',SERVICE],{stdio:['ignore','pipe','pipe']});
      child.once('error',reject);child.closed=new Promise(resolve=>child.once('close',resolve));child.stdout.once('data',data=>{child.port=Number(String(data).trim());resolve(child);});
    });}
    async function cleanup(child){if(child&&child.exitCode===null&&child.signalCode===null)child.kill('SIGKILL');if(child)await child.closed;}
    guard.luna=async()=>{if(!svc||svc.exitCode!==null||svc.signalCode!==null)svc=await start(SERVICE);return {returnValue:true};};
    const worker=require(${JSON.stringify(require.resolve('../service/update-worker'))});
    (async()=>{try{
      svc=await start(SERVICE);decoy=await start(SERVICE+'.other');
      const ops=worker.production({});
      for(const target of ['0.26.0','0.25.0']){
        const previous=svc;fs.writeFileSync(file,target);
        const old=await request('GET','http://127.0.0.1:'+svc.port+'/api/status');assert.notStrictEqual(old.json.version,target);
        await ops.elevate();
        // /proc can report exit before the parent's asynchronous child event arrives.
        await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error('old service did not exit after elevation')),1000);previous.closed.then(()=>{clearTimeout(timer);resolve();});});
        assert.strictEqual(previous.signalCode,'SIGTERM');
        await ops.health(target);assert.notStrictEqual(svc.pid,previous.pid);
        assert.strictEqual((await request('GET','http://127.0.0.1:'+svc.port+'/api/status')).json.version,target);
        assert.strictEqual(decoy.exitCode,null);assert.strictEqual(decoy.signalCode,null);
      }
      await cleanup(svc);svc=await start(SERVICE,true);const startTime=Date.now();let failed=false;
      try{await ops.elevate();}catch(e){failed=/did not stop/.test(e.message);}
      assert(failed,'an uncooperative service must fail, never pass health');assert(Date.now()-startTime<7000);assert.strictEqual(elevations,3);
      console.log('target and rollback service generations verified; decoy preserved; timeout refused');
    }finally{await cleanup(svc);await cleanup(decoy);}})().catch(e=>{console.error(e);process.exitCode=1;});`;
  fs.writeFileSync(runner, code);
  try {
    const child = cp.spawn(process.execPath, [runner, version], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', data => { output += data; }); child.stderr.on('data', data => { output += data; });
    const result = await new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    assert.deepStrictEqual(result, { code: 0, signal: null }, output);
    assert.match(output, /target and rollback service generations verified/);
  } finally { remove.rmSync(dir, { recursive: true, force: true }); }
});

test('the original failure stage survives successful rollback and a later recovery failure', async () => {
  for (const failRecovery of [false, true]) {
    const f = await fixture();
    f.ops.health = async version => { throw new Error(version === f.s.artifact.version ? 'target health mismatch' : 'backup health mismatch'); };
    if (!failRecovery) {
      const fail = f.ops.health;
      f.ops.health = async version => { if (version === f.s.artifact.version) await fail(version); };
    }
    try {
      if (failRecovery) await assert.rejects(worker.run(f.file, f.ops), /recovery could not complete/);
      else await worker.run(f.file, f.ops);
      const state = worker.load(f.file);
      assert.strictEqual(state.failure.stage, 'health-target'); assert.strictEqual(state.failure.message, 'target health mismatch');
      assert.strictEqual(typeof state.failure.at, 'number');
      if (failRecovery) { assert.strictEqual(state.recoveryFailure.stage, 'health-backup'); assert.strictEqual(state.recoveryFailure.message, 'backup health mismatch'); }
    } finally { f.close(); }
  }
});


test('startup diagnostics retain bounded command and activation evidence privately across target and backup', async () => {
  const f = await fixture(), cp = require('child_process');
  const activation = path.join(f.dir, 'activation'); mkdirSync(activation);
  fs.writeFileSync(path.join(activation, 'service'), 'target-service-definition');
  fs.writeFileSync(path.join(activation, 'manifest'), 'm'.repeat(20000));
  fs.symlinkSync(f.s.configPath, path.join(activation, 'linked'));
  const runner = path.join(f.dir, 'diagnostics.js');
  const code = `const fs=require('fs'),cp=require('child_process'),assert=require('assert'),path=require('path');
    const file=process.argv[2],dir=path.dirname(file),SERVICE=${JSON.stringify(SERVICE)},APP=${JSON.stringify(APP)};
    const mapped={};
    mapped['/var/luna-service2-dev/services.d/'+SERVICE+'.service']=path.join(dir,'activation/service');
    mapped['/var/luna-service2-dev/manifests.d/'+APP+'.json']=path.join(dir,'activation/manifest');
    mapped['/var/luna-service2/services.d/'+SERVICE+'.service']=path.join(dir,'activation/linked');
    mapped['/var/luna-service2/manifests.d/'+APP+'.json']=path.join(dir,'activation/absent');
    const realOpen=fs.openSync,realStat=fs.lstatSync,realPath=fs.realpathSync,realDirs=fs.readdirSync;
    fs.openSync=(p,...args)=>realOpen(mapped[p]||p,...args);
    fs.lstatSync=p=>realStat(mapped[p]||p);
    fs.realpathSync=p=>mapped[p]&&!realStat(mapped[p]).isSymbolicLink()?p:realPath(mapped[p]||p);
    fs.readdirSync=p=>p==='/proc'?[]:realDirs(p);
    const realExec=cp.execFile;let phase='target';
    cp.execFile=(command,args,options,done)=>{
      if(command.endsWith('/elevate-service'))return realExec(process.execPath,['-e',"process.stdout.write(process.argv[1]+'-elevation-'+ 'x'.repeat(20000));process.stderr.write('elevation-stderr')",phase],options,done);
      if(command==='script')return realExec(process.execPath,['-e',"process.stdout.write('raw-'+process.argv[1]+'-'+ 'y'.repeat(20000));process.stderr.write('ping-stderr')",phase],options,done);
      throw new Error('Unexpected command '+command);
    };
    const worker=require(${JSON.stringify(require.resolve('../service/update-worker'))});
    const state=worker.load(file),ops=worker.production(state,file),now=Date.now;
    (async()=>{for(const kind of ['target','backup']){
      phase=kind;state.installer=kind;
      fs.writeFileSync(mapped['/var/luna-service2-dev/services.d/'+SERVICE+'.service'],kind+'-service-definition');
      await ops.elevate();
      let time=0;Date.now=()=>time+=10001;
      try{await ops.health('0.26.0');throw new Error('health unexpectedly succeeded');}
      catch(e){assert.strictEqual(e.message,'Updated service health check failed (expected 0.26.0; Home recovery returned no JSON)');}
      finally{Date.now=now;}
    }
    phase='later';state.installer='target';await ops.elevate();
    // Existing records (including links) are never overwritten or followed.
    state.id='b'.repeat(24);const link=path.join(dir,state.id+'-target-elevation.json');
    fs.symlinkSync(state.configPath,link);const config=fs.readFileSync(state.configPath,'utf8');
    await ops.elevate();assert.strictEqual(fs.readFileSync(state.configPath,'utf8'),config);assert(fs.lstatSync(link).isSymbolicLink());
    if(process.platform!=='win32'){
      state.id='c'.repeat(24);fs.chmodSync(dir,0o755);await ops.elevate();fs.chmodSync(dir,0o700);
      assert.strictEqual(fs.existsSync(path.join(dir,state.id+'-target-elevation.json')),false);
      assert.strictEqual(fs.existsSync(path.join(dir,state.id+'-target-activation.json')),false);
    }
    console.log('diagnostics complete');})().catch(e=>{console.error(e);process.exitCode=1;});`;
  fs.writeFileSync(runner, code);
  try {
    const child = cp.spawn(process.execPath, [runner, f.file], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', b => { output += b; }); child.stderr.on('data', b => { output += b; });
    const result = await new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    assert.deepStrictEqual(result, { code: 0, signal: null }, output);
    const names = fs.readdirSync(f.dir).filter(n => n.indexOf(f.s.id + '-') === 0);
    assert.strictEqual(names.length, 6);
    for (const kind of ['target', 'backup']) {
      const get = name => {
        const file = path.join(f.dir, f.s.id + '-' + kind + '-' + name + '.json');
        if (process.platform !== 'win32') assert.strictEqual(fs.statSync(file).mode & 0o777, 0o600);
        const text = fs.readFileSync(file, 'utf8');
        assert(text.length < 100000); assert(!text.includes('private-sentinel')); return JSON.parse(text);
      };
      const elevate = get('elevation'), ping = get('ping'), activation = get('activation');
      assert(elevate.stdout.startsWith(kind + '-elevation-')); assert.strictEqual(elevate.stderr, 'elevation-stderr');
      assert.strictEqual(elevate.stdout.length, 16384); assert.strictEqual(elevate.stdoutTruncated, true); assert.strictEqual(elevate.code, 0);
      assert(ping.stdout.startsWith('raw-' + kind)); assert.strictEqual(ping.stderr, 'ping-stderr'); assert.strictEqual(ping.stdout.length, 16384);
      assert.strictEqual(activation.files.length, 4);
      assert.strictEqual(activation.files[0].contents, kind + '-service-definition');
      assert.strictEqual(activation.files[1].contents.length, 16384); assert.strictEqual(activation.files[1].truncated, true);
      assert.strictEqual(activation.files[2].error, 'not a regular canonical file'); assert.strictEqual(activation.files[3].error, 'ENOENT');
    }
    assert(!fs.readFileSync(f.file, 'utf8').includes('raw-target'));
  } finally { f.close(); }
});


test('standalone guard retains actual PTY failure evidence only when requested', { skip: process.platform !== 'linux' }, async () => {
  const cp = require('child_process'), dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-ping-diagnostic-'));
  const guard = path.join(dir, 'guard.js'), binary = path.join(dir, 'luna-fixture'), runner = path.join(dir, 'probe.js');
  fs.copyFileSync(require.resolve('../service/default-home-guard'), guard);
  fs.writeFileSync(binary, '#!/bin/sh\nprintf "raw-luna-failure\\n"\nprintf "raw-luna-stderr\\n" >&2\nexit 7\n', { mode: 0o755 });
  fs.writeFileSync(runner, `const assert=require('assert'),fs=require('fs'),guard=require('./guard');
    (async()=>{let captures=[];
      await guard.luna('luna://fixture/ping',{},d=>captures.push(d)).then(()=>assert.fail('ping unexpectedly succeeded'),e=>assert.strictEqual(e.message,'Home recovery returned no JSON'));
      assert.strictEqual(captures.length,1);assert(captures[0].stdout.includes('raw-luna-failure'));assert(captures[0].stdout.includes('raw-luna-stderr'));
      assert.strictEqual(captures[0].code,0); // script masks the CLI exit: retain the actual observed PTY outcome.
      await guard.luna('luna://fixture/ping',{},()=>{throw new Error('diagnostic storage unavailable');}).then(()=>assert.fail('ping unexpectedly succeeded'),e=>assert.strictEqual(e.message,'Home recovery returned no JSON'));
      fs.writeFileSync(process.env.HOMEPLUS_RECOVERY_LUNA,${JSON.stringify('#!/bin/sh\nprintf \'{"returnValue":true}\\n\'\n')});
      let successCaptures=0;assert.strictEqual((await guard.luna('luna://fixture/ping',{},()=>{successCaptures++;})).returnValue,true);assert.strictEqual(successCaptures,0);
      console.log('standalone PTY evidence preserved');
    })().catch(e=>{console.error(e);process.exitCode=1;});`);
  try {
    const child = cp.spawn(process.execPath, [runner], { env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: binary }), stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', b => { output += b; }); child.stderr.on('data', b => { output += b; });
    const result = await new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    assert.deepStrictEqual(result, { code: 0, signal: null }, output);
    assert(output.includes('standalone PTY evidence preserved'));
  } finally { remove.rmSync(dir, { recursive: true, force: true }); }
});


for (const blocked of ['diagnostic', 'mandatory']) test('preparation cleanup failure: ' + blocked, { skip: process.platform !== 'linux' }, async () => {
  const f = await fixture(), cp = require('child_process');
  f.s.active = false; recovery.atomic(f.file, f.s);
  const stale = path.join(f.dir, f.s.id + (blocked === 'diagnostic' ? '-target-ping.json' : '-previous.ipk'));
  fs.writeFileSync(stale, 'retained-old-evidence');
  const runner = path.join(f.dir, 'prepare-probe.js'), childRunner = path.join(f.dir, 'installed-worker-probe.js');
  fs.writeFileSync(childRunner, `const fs=require('fs'),path=require('path'),worker=require('./update-worker'),archive=require('./update-archive'),mkdir=require('./mkdir').mkdirSync;
    const file=process.argv[2],state=worker.load(file);
    process.stdin.resume();process.stdin.on('end',()=>process.exit(0));
    worker.run(file,{bootId:()=> 'runtime-v2:'+ 'f'.repeat(64),foreground:async()=>${JSON.stringify(APP)},
      installTarget:async()=>{const target=await archive.inspect(fs.readFileSync(state.targetFile),state.artifact.version);
        Object.keys(target.files).forEach(name=>{const i=archive.roots.findIndex(r=>name.indexOf(r+'/')===0),dest=path.join(state.directories[i],name.slice(archive.roots[i].length+1));mkdir(path.dirname(dest));fs.writeFileSync(dest,target.files[name].data,{mode:target.files[name].mode});});},
      installBackup:async()=>{throw new Error('unexpected rollback');},elevate:async()=>{},health:async()=>{},enter:async()=>true,restoreHome:async()=>{},fallback:async()=>{}
    }).then(()=>console.log('completed'),e=>{console.error(e);process.exit(1);});`);
  fs.writeFileSync(runner, `const fs=require('fs'),cp=require('child_process'),assert=require('assert'),path=require('path');
    const file=process.argv[2],stale=process.argv[3],blocked=process.argv[4],dir=path.dirname(file);
    const read=fs.readFileSync,stat=fs.lstatSync,mkdir=fs.mkdirSync,realpath=fs.realpathSync,access=fs.accessSync,unlink=fs.unlinkSync;
    const boot='/dev/homeplus-update-boot',service=${JSON.stringify(SERVICE)};
    fs.readFileSync=(p,...args)=>p==='/proc/self/mountinfo'?'1 2 0:1 / /dev rw - tmpfs tmpfs rw':p===boot+'/generation.json'?JSON.stringify({schemaVersion:1,generation:'f'.repeat(64)}):read(p,...args);
    fs.lstatSync=p=>p==='/dev'||p===boot?{uid:0,mode:0o700,isDirectory:()=>true,isSymbolicLink:()=>false}:p===boot+'/generation.json'?{uid:0,mode:0o600,size:120,isFile:()=>true}:stat(p);
    fs.mkdirSync=(p,...args)=>{if(p===boot){const e=new Error('exists');e.code='EEXIST';throw e;}return mkdir(p,...args);};
    fs.realpathSync=p=>p==='/dev'?p:realpath(p);
    fs.accessSync=(p,...args)=>p.endsWith('/elevate-service')||p==='/usr/bin/luna-send'?undefined:access(p,...args);
    fs.unlinkSync=p=>{if(p===stale){const e=new Error('injected stale unlink failure');e.code='EACCES';throw e;}return unlink(p);};
    const release=require(${JSON.stringify(require.resolve('../service/update-release'))});
    const prior=JSON.parse(read(file)),target=read(prior.targetFile);release.download=async()=>target;
    const guard=require(${JSON.stringify(require.resolve('../service/default-home-guard'))});let launched=0,child,closed,completed;
    guard.luna=async(uri,params)=>{
      if(uri.endsWith('/getConfiguration'))return {returnValue:true,root:true};
      assert(uri.endsWith('/exec'));
      if(params.command.indexOf(' --start ')>=0){launched++;child=cp.spawn(process.execPath,[path.join(dir,'installed-worker-probe.js'),file],{stdio:['pipe','pipe','pipe']});
        let output='';child.stdout.on('data',b=>{output+=b;});child.stderr.on('data',b=>{output+=b;});
        closed=new Promise(resolve=>child.once('close',(code,signal)=>resolve({code,signal,output})));
        completed=new Promise((resolve,reject)=>{child.stdout.on('data',b=>{if(String(b).includes('completed'))resolve();});child.once('exit',code=>{if(code)reject(new Error(output));});});
      }return {returnValue:true};
    };
    const worker=require(${JSON.stringify(require.resolve('../service/update-worker'))});
    (async()=>{let failure;
      try{await worker.prepare({isRoot:()=>true,stateDir:dir,appDir:prior.directories[0],serviceDir:prior.directories[1],packageDir:prior.directories[2],configPath:prior.configPath,initDir:dir,
        readConfig:()=>JSON.parse(read(prior.configPath)),writeConfig:()=>{throw new Error('unexpected config write');},foreground:()=>true,version:prior.previousVersion,artifact:prior.artifact});}catch(e){failure=e;}
      if(blocked==='diagnostic'){
        assert.ifError(failure);assert.strictEqual(launched,1);await completed;assert.strictEqual(worker.load(file).phase,'completed');assert.strictEqual(worker.load(file).active,false);
      }else{assert(failure);assert.strictEqual(failure.code,'EACCES');assert.strictEqual(launched,0);}
      assert.strictEqual(read(stale,'utf8'),'retained-old-evidence');
      if(child){child.stdin.end();const result=await closed;assert.strictEqual(result.code,0,result.output);assert.strictEqual(result.signal,null);}
      console.log(blocked+' cleanup outcome verified');
    })().catch(e=>{console.error(e);if(child)child.kill('SIGKILL');process.exitCode=1;});`);
  try {
    const child = cp.spawn(process.execPath, [runner, f.file, stale, blocked], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', b => { output += b; }); child.stderr.on('data', b => { output += b; });
    const result = await new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    assert.deepStrictEqual(result, { code: 0, signal: null }, output);
    assert(output.includes(blocked + ' cleanup outcome verified'));
  } finally { f.close(); }
});
