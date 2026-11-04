'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const remove = require('../service/remove');
const fs = require('fs');
const os = require('os');
const path = require('path');
const createHome = require('../service/default-home');
const guard = require('../service/default-home-guard');
const APP = 'com.github.pbalazaar.homeplus', LG = 'com.webos.app.home';

function fixture(flags = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-system-home-'));
  const app = path.join(dir, 'app'), runtime = path.join(dir, 'runtime'), file = path.join(dir, 'config.json');
  fs.mkdirSync(app); fs.mkdirSync(runtime);
  fs.writeFileSync(path.join(app, 'appinfo.json'), JSON.stringify({ id: APP, appCategory: ['home'] }));
  fs.writeFileSync(path.join(app, 'homeplus'), 'native fixture');
  const initial = { account: { token: 'keep' }, prefs: { brightness: 80 }, homeMode: 'watch', homeOverride: false };
  fs.writeFileSync(file, JSON.stringify(initial));
  const readConfig = () => JSON.parse(fs.readFileSync(file, 'utf8'));
  const writeConfig = (c) => fs.writeFileSync(file, JSON.stringify(c));
  const model = { defaults: flags.absent ? { browser: 'browser' } : { home: LG, browser: 'browser' }, foreground: APP, calls: [] };
  const before = Object.assign({}, model.defaults);
  let recovering = false, reads = 0, starts = 0;
  const luna = async (uri, p) => {
    model.calls.push({ uri, p });
    const method = uri.slice(uri.lastIndexOf('/') + 1);
    if (method === 'getProfiles') {
      assert.deepEqual(p.names, ['defaultApps']);
      return flags.oldProfile ? { returnValue: false, errorText: 'Service does not exist' } :
        { returnValue: true, profiles: { defaultApps: { support: !flags.unsupported, categories: { home: LG } } } };
    }
    if (method === 'getAppInfo') return { returnValue: true, appInfo: { id: p.id, appCategory: flags.noCategory && p.id === APP ? [] : ['home'] } };
    if (method === 'getSystemSettings') {
      reads++;
      const snapshot = Object.assign({}, model.defaults);
      if (flags.expireOnFinalRead && reads === 3) {
        const stateFile = path.join(dir, 'guard/state.json'), s = guard.load(stateFile);
        s.active = false; guard.atomic(stateFile, s); model.defaults.home = LG;
      }
      return flags.unreadable || (recovering && flags.badRollback) ? { returnValue: false } :
        { returnValue: true, settings: { defaultApps: snapshot } };
    }
    if (method === 'setDefaultApp') {
      if (flags.denied && p.appId === APP) return { returnValue: false, errorText: 'Denied' };
      if (!flags.ignored) model.defaults.home = p.appId;
      return { returnValue: true };
    }
    if (method === 'setSystemSettings') { model.defaults = p.settings.defaultApps; return { returnValue: true }; }
    if (method === 'launch') {
      if (flags.refusedStartup && p.id === APP) return { returnValue: false, errorText: 'Startup refused' };
      model.foreground = p.id; return { returnValue: true };
    }
    if (method === 'getForegroundAppInfo') return { returnValue: true, appId: model.foreground };
    if (method === 'launchDefaultApp') {
      if (flags.badRollback) { recovering = true; return { returnValue: false, errorText: 'Launch failed' }; }
      if (flags.refusedLaunch) return { returnValue: false, errorText: 'Launch failed' };
      if (!flags.wrongForeground) model.foreground = model.defaults.home || LG;
      if (flags.otherHome) model.defaults.home = 'another.home';
      if (flags.changeAccount) { const c = readConfig(); c.account.token = 'new'; c.prefs.brightness = 60; writeConfig(c); }
      return { returnValue: true };
    }
    throw new Error('Unexpected Luna ' + uri);
  };
  const home = createHome({
    luna, readConfig, writeConfig, configPath: () => file, appDir: app, initDir: path.join(dir, 'init.d'),
    stateDir: path.join(dir, 'guard'), runtimeDir: runtime, isRoot: () => !flags.unrooted, timeout: 100,
    suspend: async () => { const c = readConfig(); c.homeOverride = false; writeConfig(c); },
    resume: async () => {}, log: () => {},
    startGuard: async () => {
      starts++;
      if (flags.slowRecovery && starts === 2) await new Promise((resolve) => setTimeout(resolve, 100));
      if (flags.noGuard) throw new Error('Recovery did not start');
    }
  });
  return { dir, app, file, before, initial, model, luna, home, readConfig, writeConfig,
    state: () => guard.load(path.join(dir, 'guard/state.json')), starts: () => starts,
    close: () => remove.rmSync(dir, { recursive: true, force: true }) };
}

test('startup waits for delayed recovery instead of silently dropping the boot request', async () => {
  const t = fixture({ slowRecovery: true });
  try {
    await t.home.setEnabled(true);
    const cfg = t.readConfig(); cfg.startupEnabled = true; t.writeConfig(cfg);
    t.model.foreground = 'com.webos.app.livetv';
    const recovering = t.home.recover(), starting = t.home.startup();
    await recovering;
    assert.deepEqual(await starting, { launched: true });
    assert.equal(t.model.foreground, APP);
  } finally { t.close(); }
});
+
test('expiry during the final read cannot promote a stale trial', async () => {
  const t = fixture({ expireOnFinalRead: true });
  try {
    await assert.rejects(t.home.setEnabled(true), /recovery window/);
    assert.equal(t.state().active, false);
    assert.deepEqual(t.model.defaults, t.before);
    assert.deepEqual(t.readConfig(), t.initial);
  } finally { t.close(); }
});
test('startup failure restores an active default before reporting the error', async () => {
  const t = fixture({ refusedStartup: true });
  try {
    await t.home.setEnabled(true);
    const cfg = t.readConfig(); cfg.startupEnabled = true; t.writeConfig(cfg);
    t.model.foreground = 'com.webos.app.livetv';
    await assert.rejects(t.home.startup(), /Startup refused/);
    assert.deepEqual(t.model.defaults, t.before);
    assert.equal(t.state().active, false);
    assert.equal(t.readConfig().defaultHomeEnabled, undefined);
    assert.equal(t.readConfig().account.token, 'keep');
    assert.equal(t.readConfig().startupEnabled, true);
  } finally { t.close(); }
});
test('service restart retains a confirmed default and does not renew a trial', async () => {
  const t = fixture();
  try {
    await t.home.setEnabled(true);
    const original = t.state();
    await t.home.recover();
    assert.equal(t.starts(), 2);
    assert.equal(t.state().deadline, null);
    assert.equal(t.state().transaction, original.transaction);
    assert.equal(t.readConfig().defaultHomeEnabled, true);
    const pending = t.state(); pending.deadline = Date.now() + 30000;
    guard.atomic(path.join(t.dir, 'guard/state.json'), pending);
    await t.home.recover();
    assert.equal(t.state().active, false);
    assert.deepEqual(t.model.defaults, t.before);
    assert.deepEqual(t.readConfig(), t.initial);
  } finally { t.close(); }
});
test('removal cannot delete recovery while an enable request is in flight', async () => {
  const t = fixture();
  try {
    const enabling = t.home.setEnabled(true);
    await assert.rejects(t.home.remove(), /already in progress/);
    await enabling;
    assert.equal(t.state().active, true);
    await t.home.remove();
    assert.deepEqual(t.model.defaults, t.before);
    assert.equal(fs.existsSync(path.join(t.dir, 'guard/state.json')), false);
  } finally { t.close(); }
});
for (const flags of [{ unsupported: true }, { oldProfile: true }, { noCategory: true }, { unreadable: true }, { unrooted: true }]) {
  test('unsupported preflight leaves every setting alone: ' + JSON.stringify(flags), async () => {
    const t = fixture(flags);
    try {
      assert.equal((await t.home.status()).defaultHomeAvailable, false);
      await assert.rejects(t.home.setEnabled(true));
      assert.deepEqual(t.model.defaults, t.before);
      assert.deepEqual(t.readConfig(), t.initial);
      assert.equal(t.model.calls.filter((c) => /setDefaultApp|setSystemSettings/.exec(c.uri)).length, 0);
    } finally { t.close(); }
  });
}
for (const flags of [{ denied: true }, { ignored: true }, { refusedLaunch: true }, { wrongForeground: true }, { noGuard: true }]) {
  test('a failed guarded trial restores the exact snapshot: ' + JSON.stringify(flags), async () => {
    const t = fixture(flags);
    try {
      await assert.rejects(t.home.setEnabled(true));
      assert.deepEqual(t.model.defaults, t.before);
      assert.deepEqual(t.readConfig(), t.initial);
      assert.equal(t.state().active, false);
    } finally { t.close(); }
  });
}
test('a successful trial starts from another foreground before resolving the default', async () => {
  const t = fixture({ absent: true, changeAccount: true });
  try {
    await t.home.setEnabled(true);
    assert.equal(t.model.defaults.home, APP);
    assert.equal(t.state().deadline, null);
    assert.equal(t.readConfig().account.token, 'new');
    assert.equal(t.readConfig().prefs.brightness, 60);
    const launches = t.model.calls.filter((c) => /\/launch$|\/launchDefaultApp$/.exec(c.uri));
    assert.equal(launches[0].p.id, LG);
    assert.equal(launches[1].p.category, 'home');
    t.model.defaults.browser = 'new.browser';
    await t.home.setEnabled(false);
    assert.deepEqual(t.model.defaults, { browser: 'new.browser' });
    assert.equal(t.readConfig().homeOverride, false);
    assert.equal(t.readConfig().account.token, 'new');
  } finally { t.close(); }
});
test('another Home chosen during the trial is preserved', async () => {
  const t = fixture({ otherHome: true });
  try {
    await assert.rejects(t.home.setEnabled(true), /changed/);
    assert.equal(t.model.defaults.home, 'another.home');
    assert.deepEqual(t.readConfig(), t.initial);
  } finally { t.close(); }
});
test('uncertain recovery stays active instead of guessing or reporting success', async () => {
  const t = fixture({ badRollback: true });
  try {
    await assert.rejects(t.home.setEnabled(true), /independent recovery remains active/);
    assert.equal(t.state().active, true);
    assert.equal(t.model.defaults.home, APP);
  } finally { t.close(); }
});
test('startup is independent of Home button takeover and does not steal another app', async () => {
  const t = fixture();
  try {
    assert.deepEqual(await t.home.startup(), { launched: false });
    const cfg = t.readConfig(); cfg.startupEnabled = true; t.writeConfig(cfg);
    t.model.foreground = 'netflix';
    assert.deepEqual(await t.home.startup(), { launched: false });
    assert.equal(t.model.foreground, 'netflix');
    fs.unlinkSync(path.join(t.dir, 'runtime/homeplus-startup-requested'));
    t.model.foreground = 'com.webos.app.hdmi2';
    assert.deepEqual(await t.home.startup(), { launched: true });
    assert.equal(t.model.foreground, APP);
    assert.deepEqual(await t.home.startup(), { launched: false });
    assert.equal(t.readConfig().homeOverride, false);
  } finally { t.close(); }
});
