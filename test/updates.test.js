'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { createUpdates, compareVersions, validateArtifact } = require('../service/updates');
const { configStore, until } = require('./helpers');
const remove = require('../service/remove');
const APP = 'com.github.pbalazaar.homeplus';
const release = (version) => ({ repository: 'pbalazaar/home-plus', version, tag: 'v' + version, commit: 'a'.repeat(40), notes: 'Release notes', artifact: { version, size: 1024, sha256: 'b'.repeat(64), url: 'https://github.com/pbalazaar/home-plus/releases/download/v' + version + '/' + APP + '_' + version + '_all.ipk' } });
function fixture(extra) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-updates-'));
  const store = configStore();
  const options = Object.assign({ version: '0.25.0', appDir: dir, serviceDir: dir, stateDir: path.join(dir, 'state'), readConfig: store.readConfig, writeConfig: store.writeConfig,
    fetchRelease: async () => ({ upstream: release('0.26.0'), result: null }), launchWorker: async () => {}, isRoot: () => true, foreground: () => true }, extra);
  return { options, store, dir, close() { remove.rmSync(dir, { recursive: true, force: true }); } };
}
async function check(c) { await c.act({ action: 'check' }); await until(() => c.state().phase !== 'checking'); return c.state(); }
test('numeric versions and strict artifact identity', () => {
  assert(compareVersions('0.9.0', '0.10.0') < 0);
  assert.throws(() => compareVersions('1.0.0-beta', '1.0.0'));
  const a = release('0.26.0').artifact;
  assert(validateArtifact(a, 'pbalazaar/home-plus', 'v0.26.0'));
  for (const change of [{ url: a.url.replace('github.com', 'evil.example') }, { sha256: 'bad' }, { size: 0 }, { url: a.url.replace(APP, 'another.app') }])
    assert.throws(() => validateArtifact(Object.assign({}, a, change), 'pbalazaar/home-plus', 'v0.26.0'));
});
test('keep persists across restart, changed release hash and manual checks; newer release prompts', async () => {
  const f = fixture(); let c = createUpdates(f.options);
  try {
    let s = await check(c); assert.strictEqual(s.prompt, true); assert.match(s.warning, /unknown|unverified/i);
    await c.act({ action: 'keep', version: s.upstreamVersion }); c.close();
    const changed = release('0.26.0'); changed.artifact.sha256 = 'c'.repeat(64);
    f.options.fetchRelease = async () => ({ upstream: changed, result: null }); c = createUpdates(f.options);
    s = await check(c); assert.strictEqual(s.prompt, false); assert.strictEqual(s.ignoredVersion, '0.26.0');
    f.options.fetchRelease = async () => ({ upstream: release('0.27.0'), result: null }); c.close(); c = createUpdates(f.options);
    s = await check(c); assert.strictEqual(s.prompt, true);
    await assert.rejects(c.act({ action: 'keep', version: '0.26.0' }), /changed|current/i);
  } finally { c.close(); f.close(); }
});
test('exact confirmation is required; changed metadata prevents install', async () => {
  let calls = 0, launches = 0;
  const f = fixture({ fetchRelease: async () => { const r = release('0.26.0'); if (++calls > 1) r.artifact.sha256 = 'c'.repeat(64); return { upstream: r }; }, launchWorker: async () => { launches++; } });
  const c = createUpdates(f.options);
  try {
    const s = await check(c);
    await assert.rejects(c.act({ action: 'install', version: s.upstreamVersion, target: 'upstream', confirmation: 'bad' }), /confirmation/i);
    await c.act({ action: 'install', version: s.upstreamVersion, target: 'upstream', confirmation: s.confirmation });
    await until(() => c.state().phase !== 'installing');
    assert.strictEqual(launches, 0); assert.match(c.state().message, /changed/i);
  } finally { c.close(); f.close(); }
});
test('automatic off starts no check and manual check remains available', async () => {
  let checks = 0; const f = fixture({ fetchRelease: async () => { checks++; return { upstream: release('0.26.0') }; }, startupDelay: 1 });
  const c = createUpdates(f.options);
  try { await c.act({ action: 'automatic', enabled: false }); c.start(); await new Promise(r => setTimeout(r, 25)); assert.strictEqual(checks, 0); await check(c); assert.strictEqual(checks, 1); }
  finally { c.close(); f.close(); }
});
test('known inventory permits only the generated base36 QML link and detects edits', () => {
  const f = fixture(), a = path.join(f.dir, 'app'), s = path.join(f.dir, 'service'), crypto = require('crypto');
  fs.mkdirSync(a); fs.mkdirSync(s); fs.mkdirSync(path.join(a, 'qml'));
  fs.writeFileSync(path.join(a, 'qml', 'ControlCenter.qml'), 'qml payload'); fs.writeFileSync(path.join(s, 'service.js'), 'service payload');
  const info = { schemaVersion: 1, upstream: { repository: 'pbalazaar/home-plus', version: '0.25.0', commit: 'a'.repeat(40) }, source: { repository: 'MoBluey/home-plus', commit: 'b'.repeat(40) }, customised: true, customisations: ['updater'], fileHashes: {} };
  info.fileHashes['application/qml/ControlCenter.qml'] = crypto.createHash('sha256').update('qml payload').digest('hex');
  info.fileHashes['service/service.js'] = crypto.createHash('sha256').update('service payload').digest('hex');
  fs.writeFileSync(path.join(a, 'update-info.json'), JSON.stringify(info)); fs.symlinkSync('qml', path.join(a, 'qml-mvzr401k'));
  const provenance = require('../service/updates').provenance;
  try {
    assert.strictEqual(provenance(a, s).known, true);
    fs.unlinkSync(path.join(a, 'qml-mvzr401k')); fs.symlinkSync('../service', path.join(a, 'qml-mvzr401k'));
    assert.strictEqual(provenance(a, s).known, false);
    fs.unlinkSync(path.join(a, 'qml-mvzr401k')); fs.writeFileSync(path.join(s, 'service.js'), 'edited');
    assert.strictEqual(provenance(a, s).known, false);
  } finally { f.close(); }
});
function known(f, repository) {
  const app = path.join(f.dir, 'application'), svc = path.join(f.dir, 'service');
  fs.mkdirSync(app); fs.mkdirSync(svc); fs.writeFileSync(path.join(app, 'payload'), 'known');
  const hash = require('crypto').createHash('sha256').update('known').digest('hex');
  fs.writeFileSync(path.join(app, 'update-info.json'), JSON.stringify({ schemaVersion: 1, upstream: { repository: 'pbalazaar/home-plus', version: '0.25.0', commit: 'a'.repeat(40) }, source: { repository, commit: 'c'.repeat(40) }, customised: repository !== 'pbalazaar/home-plus', customisations: [], fileHashes: { 'application/payload': hash } }));
  f.options.appDir = app; f.options.serviceDir = svc;
}
test('verified official and unknown builds use only upstream; known fork selects its fixed result feed', async () => {
  for (const repository of [null, 'pbalazaar/home-plus', 'MoBluey/home-plus', 'Example-Owner/custom.home-plus']) {
    let feed;
    const f = fixture({ fetchRelease: async value => { feed = value; return { upstream: release('0.26.0') }; } });
    if (repository) known(f, repository);
    const c = createUpdates(f.options);
    try { const s = await check(c); assert.strictEqual(feed, repository && repository !== 'pbalazaar/home-plus' ? repository : null); assert.strictEqual(s.canInstallPreserved, repository === 'pbalazaar/home-plus'); }
    finally { c.close(); f.close(); }
  }
});
test('keep survives a custom conflict becoming ready and cached checks respect daily cadence', async () => {
  let status = 'conflict', count = 0;
  const f = fixture({ startupDelay: 1, fetchRelease: async () => {
    count++; const upstream = release('0.26.0'); upstream.repository = 'pbalazaar/home-plus';
    const result = { schemaVersion: 1, upstream, source: { repository: 'MoBluey/home-plus', commit: 'c'.repeat(40), base: 'a'.repeat(40) }, status, message: status, conflicts: [], customisations: ['updater'] };
    if (status === 'ready') result.artifact = Object.assign({}, upstream.artifact, { url: 'https://github.com/MoBluey/home-plus/releases/download/upstream-v0.26.0/' + APP + '_0.26.0_cccccccccccc_bbbbbbbbbbbb_all.ipk' });
    return { upstream, result };
  } });
  known(f, 'MoBluey/home-plus'); let c = createUpdates(f.options);
  try {
    assert.strictEqual((await check(c)).phase, 'conflict'); await c.act({ action: 'keep', version: '0.26.0' }); c.close(); status = 'ready';
    c = createUpdates(f.options); c.start(); await new Promise(r => setTimeout(r, 25)); assert.strictEqual(count, 1);
    const s = await check(c); assert.strictEqual(s.phase, 'ready'); assert.strictEqual(s.canInstallPreserved, true); assert.strictEqual(s.prompt, false);
  } finally { c.close(); f.close(); }
});
test('older releases never install and duplicate installation requests are refused', async () => {
  const old = fixture({ fetchRelease: async () => ({ upstream: release('0.24.0') }) }); const oldController = createUpdates(old.options);
  try { const s = await check(oldController); assert.strictEqual(s.canInstallUpstream, false); assert.strictEqual(s.prompt, false); } finally { oldController.close(); old.close(); }
  let finish;
  const f = fixture({ launchWorker: () => new Promise(resolve => { finish = resolve; }) }), c = createUpdates(f.options);
  try {
    const s = await check(c), action = { action: 'install', version: s.upstreamVersion, confirmation: s.confirmation, target: 'upstream' };
    await c.act(action); await assert.rejects(c.act(action), /already running/); await until(() => !!finish); finish();
  } finally { c.close(); f.close(); }
});
test('corrupt durable transaction fails closed and retains the previous backup', async () => {
  const f = fixture(), c = createUpdates(f.options);
  try {
    const s = await check(c); fs.mkdirSync(f.options.stateDir); fs.writeFileSync(path.join(f.options.stateDir, 'transaction.json'), '{');
    assert.strictEqual(c.busy(), true); assert.strictEqual(c.state().phase, 'recovering');
    await assert.rejects(c.act({ action: 'install', version: s.upstreamVersion, confirmation: s.confirmation, target: 'upstream' }), /already running/);
  } finally { c.close(); f.close(); }
});
test('installed edits made after the prompt invalidate consent', async () => {
  let launched = false;
  const f = fixture({ launchWorker: async () => { launched = true; } }); known(f, 'pbalazaar/home-plus');
  const c = createUpdates(f.options);
  try {
    const s = await check(c);
    fs.writeFileSync(path.join(f.options.appDir, 'payload'), 'an unknown edit');
    await c.act({ action: 'install', version: s.upstreamVersion, confirmation: s.confirmation, target: 'preserve' });
    await until(() => c.state().phase !== 'installing');
    assert.strictEqual(launched, false); assert.match(c.state().message, /changed/);
  } finally { c.close(); f.close(); }
});

test('known provenance ignores only a regular application PulseAudio cookie, including legacy hashes', () => {
  const f = fixture(); known(f, 'MoBluey/home-plus');
  const app = f.options.appDir, svc = f.options.serviceDir, cookie = path.join(app, '.config/pulse/cookie');
  const provenance = require('../service/updates').provenance, read = fs.readFileSync;
  require('../service/mkdir').mkdirSync(path.dirname(cookie)); fs.writeFileSync(cookie, Buffer.alloc(256, 7)); fs.chmodSync(cookie, 0o777);
  try {
    fs.readFileSync = function (file) { if (file === cookie) throw new Error('Runtime cookie contents must not be read'); return read.apply(this, arguments); };
    assert.strictEqual(provenance(app, svc).known, true);
    const infoPath = path.join(app, 'update-info.json'), info = JSON.parse(read(infoPath));
    info.fileHashes['application/.config/pulse/cookie'] = 'd'.repeat(64); fs.writeFileSync(infoPath, JSON.stringify(info));
    assert.strictEqual(provenance(app, svc).known, true);
    fs.unlinkSync(cookie);
    assert.strictEqual(provenance(app, svc).known, true);
    fs.symlinkSync(path.join(app, 'payload'), cookie);
    assert.strictEqual(provenance(app, svc).known, false);
    fs.unlinkSync(cookie); fs.mkdirSync(cookie);
    assert.strictEqual(provenance(app, svc).known, false);
    fs.rmdirSync(cookie);
    for (const file of [path.join(app, '.config/pulse/extra.js'), path.join(app, '.config/extra.js'), path.join(svc, '.config/pulse/cookie')]) {
      require('../service/mkdir').mkdirSync(path.dirname(file)); fs.writeFileSync(file, 'unexpected code');
      assert.strictEqual(provenance(app, svc).known, false); fs.unlinkSync(file);
    }
    fs.writeFileSync(path.join(app, 'payload'), 'unknown edit');
    assert.strictEqual(provenance(app, svc).known, false);
  } finally { fs.readFileSync = read; f.close(); }
});

test('preserved results and assets must match the installed fork, regardless of request or settings', async () => {
  for (const repository of ['MoBluey/home-plus', 'Example-Owner/custom.home-plus']) {
    let feed, mismatch = '';
    const f = fixture({ fetchRelease: async value => {
      feed = value;
      const upstream = release('0.26.0'); upstream.repository = 'pbalazaar/home-plus';
      const source = { repository: mismatch === 'source' ? 'Other/fork' : repository, commit: 'c'.repeat(40), base: 'a'.repeat(40) };
      return { upstream, result: { schemaVersion: 1, upstream, source, status: 'ready', conflicts: [], customisations: ['service/custom.js'],
        artifact: Object.assign({}, upstream.artifact, { url: 'https://github.com/' + (mismatch === 'asset' ? 'Other/fork' : repository) + '/releases/download/upstream-v0.26.0/' + APP + '_0.26.0_cccccccccccc_bbbbbbbbbbbb_all.ipk' }) } };
    } });
    known(f, repository);
    f.store.writeConfig({ updates: { repository: 'Other/fork', feed: 'Other/fork' } });
    const c = createUpdates(f.options);
    try {
      await c.act({ action: 'check', repository: 'Other/fork', url: 'https://evil.invalid' });
      await until(() => c.state().phase !== 'checking');
      assert.strictEqual(feed, repository); assert.strictEqual(c.state().canInstallPreserved, true);
      for (const change of ['source', 'asset']) { mismatch = change; const s = await check(c); assert.strictEqual(s.phase, 'error'); assert.strictEqual(s.canInstallPreserved, false); }
    } finally { c.close(); f.close(); }
  }
});
