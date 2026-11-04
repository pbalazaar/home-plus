/* System Home is opt-in. Check the TV, trial its resolver, and keep recovery outside our package. */
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');
const recovery = require('./default-home-guard');
const { mkdirSync } = require('./mkdir');
const APP = 'com.github.pbalazaar.homeplus';
const MANAGER = 'luna://com.webos.applicationManager/';
const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const quote = (s) => "'" + s.replace(/'/g, "'\\''") + "'";

module.exports = function defaultHome(options) {
  const { luna, readConfig, writeConfig, configPath, appDir, initDir, stateDir, isRoot, suspend, resume, log } = options;
  const file = path.join(stateDir, 'state.json'), helper = path.join(stateDir, 'guard.js');
  const hook = path.join(initDir, '00-homeplus-default-home'), boot = path.join(stateDir, 'boot.sh');
  const timeout = options.timeout || 4000;
  let busy = false, capability = null, checkedAt = 0, recoveryRun = null;
  async function call(uri, params) { return recovery.accepted(luna, uri, params); }
  async function inspect(uri, params) {
    let timer;
    try {
      return await Promise.race([luna(uri, params), new Promise((resolve) => {
        timer = setTimeout(() => resolve({ returnValue: false, errorText: 'The TV did not answer its Home compatibility check' }), 2000);
      })]);
    } finally { clearTimeout(timer); }
  }
  async function preflight() {
    if (!isRoot()) throw new Error('Homebrew Channel root access is needed');
    const profile = await recovery.accepted(inspect, 'luna://com.webos.service.systemprofile/getProfiles', { names: ['defaultApps'], subscribe: false });
    const support = profile.profiles && profile.profiles.defaultApps;
    if (!support || support.support !== true || !support.categories || !support.categories.home)
      throw new Error('This TV has not reported support for a default Home app');
    const stockHome = support.categories.home;
    if (typeof stockHome !== 'string' || !/^[a-zA-Z0-9._-]+$/.exec(stockHome) || stockHome === APP) throw new Error('The TV did not report a recoverable LG Home app');
    const own = await recovery.accepted(inspect, MANAGER + 'getAppInfo', { id: APP });
    if (!own.appInfo || !Array.isArray(own.appInfo.appCategory) || own.appInfo.appCategory.indexOf('home') === -1)
      throw new Error('Home+ is not registered as a Home app');
    const stock = await recovery.accepted(inspect, MANAGER + 'getAppInfo', { id: stockHome });
    if (!stock.appInfo || stock.appInfo.id !== stockHome) throw new Error('LG Home is not available for recovery');
    const before = await recovery.readDefaults(inspect);
    if (before.home === APP && !readConfig().defaultHomeEnabled) throw new Error('Home+ is already default without a recovery snapshot');
    return { before, stockHome };
  }
  async function status() {
    if (!capability || Date.now() - checkedAt > 30000) {
      try { await preflight(); capability = { available: true, reason: '' }; }
      catch (e) { capability = { available: false, reason: e.message }; }
      checkedAt = Date.now();
    }
    return { defaultHomeEnabled: readConfig().defaultHomeEnabled === true,
      defaultHomeAvailable: capability.available, defaultHomeReason: capability.reason,
      startupEnabled: readConfig().startupEnabled === true };
  }
  function state() {
    try { return recovery.load(file); } catch (e) { if (e.code === 'ENOENT') return null; throw e; }
  }
  async function startGuard(s) {
    if (options.startGuard) return options.startGuard(s, file);
    try {
      const ready = JSON.parse(fs.readFileSync(file + '.ready', 'utf8'));
      if (ready.transaction === s.transaction) {
        process.kill(ready.pid, 0);
        if (process.platform !== 'linux') return;
        const argv = fs.readFileSync('/proc/' + ready.pid + '/cmdline', 'utf8').split(String.fromCharCode(0));
        if (argv[1] === helper && argv[2] === '--watch' && argv[3] === file) return;
      }
    } catch (e) { /* absent, or left over from another boot */ }
    try { fs.unlinkSync(file + '.ready'); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    const out = fs.openSync(file + '.log', 'a', 0o600);
    const child = spawn(process.execPath, [helper, '--watch', file], { detached: true, stdio: ['ignore', out, out] });
    child.unref(); fs.closeSync(out);
    const end = Date.now() + 4000;
    while (Date.now() < end) {
      try {
        const ready = JSON.parse(fs.readFileSync(file + '.ready', 'utf8'));
        if (ready.pid === child.pid && ready.transaction === s.transaction) { process.kill(child.pid, 0); return; }
      } catch (e) { /* starting */ }
      await wait(50);
    }
    throw new Error('Independent Home recovery did not start');
  }
  function stage(s) {
    mkdirSync(stateDir, 0o700); fs.chmodSync(stateDir, 0o700); mkdirSync(initDir);
    let linked = false;
    try {
      if (fs.readlinkSync(hook) !== boot) throw new Error('Home recovery startup path is already in use');
      linked = true;
    } catch (e) {
      if (e.code !== 'ENOENT') throw e;
    }
    recovery.durable(helper, fs.readFileSync(path.join(__dirname, 'default-home-guard.js')));
    recovery.atomic(file, s);
    recovery.durable(boot, '#!/bin/sh\n/usr/bin/node ' + quote(helper) + ' --start ' + quote(file) + '\n', 0o700);
    if (!linked) fs.symlinkSync(boot, hook);
    recovery.syncDir(initDir);
  }
  function unlinkHook() {
    try { if (fs.readlinkSync(hook) === boot) fs.unlinkSync(hook); } catch (e) { if (e.code !== 'ENOENT') throw e; }
  }
  async function restore(s) {
    const result = await recovery.restore(luna, s.before, s.stockHome);
    recovery.restoreConfig(s);
    s.active = false; s.result = result; recovery.atomic(file, s); unlinkHook();
    await resume();
    return result;
  }
  async function disable() {
    const s = state();
    if (!s) {
      if (readConfig().defaultHomeEnabled) throw new Error('Original Home snapshot is missing; recovery is required');
      return;
    }
    if (s.active || readConfig().defaultHomeEnabled) await restore(s);
    else unlinkHook();
  }
  async function foreground(id) {
    const end = Date.now() + timeout;
    do {
      const r = await call(MANAGER + 'getForegroundAppInfo', {});
      const app = r.appId || (r.foregroundAppInfo && r.foregroundAppInfo[0] && r.foregroundAppInfo[0].appId);
      if (app === id) return;
      await wait(75);
    } while (Date.now() < end);
    throw new Error('The TV did not open the requested Home app');
  }
  async function setEnabled(enabled) {
    if (busy) throw new Error('A Home change is already in progress');
    busy = true;
    let s;
    try {
      if (!enabled) { await disable(); return; }
      state(); // refuse unreadable recovery metadata rather than replacing it
      if (readConfig().defaultHomeEnabled) {
        const current = await recovery.readDefaults(luna);
        if (current.home === APP) return;
        throw new Error('The default Home app changed outside Home+');
      }
      const probe = await preflight(), cfg = readConfig(), previous = {};
      for (const key of ['homeMode', 'homeOverride', 'defaultHomeEnabled']) if (Object.prototype.hasOwnProperty.call(cfg, key)) previous[key] = cfg[key];
      s = { version: 1, active: true, deadline: Date.now() + 30000, transaction: crypto.randomBytes(12).toString('hex'),
        applicationDir: appDir, configPath: configPath(), previous, before: probe.before, stockHome: probe.stockHome };
      stage(s); await startGuard(s);
      await suspend();
      // Observing another app first prevents an already-visible Home+ from passing the check.
      await call(MANAGER + 'launch', { id: s.stockHome }); await foreground(s.stockHome);
      if (!state().active || Date.now() + 5000 >= s.deadline) throw new Error('The Home check exceeded its recovery window');
      await call(MANAGER + 'setDefaultApp', { category: 'home', appId: APP });
      if ((await recovery.readDefaults(luna)).home !== APP) throw new Error('The TV did not retain the default Home setting');
      if (!state().active || Date.now() + 5000 >= s.deadline) throw new Error('The Home check exceeded its recovery window');
      await call(MANAGER + 'launchDefaultApp', { category: 'home' }); await foreground(APP);
      if ((await recovery.readDefaults(luna)).home !== APP) throw new Error('The default Home app changed during the check');
      const fresh = state();
      if (!fresh.active || fresh.transaction !== s.transaction || Date.now() + 5000 >= fresh.deadline)
        throw new Error('The compatibility check exceeded its recovery window');
      const next = readConfig(); next.defaultHomeEnabled = true; next.homeOverride = true; next.homeMode = 'watch';
      writeConfig(next);
      fresh.deadline = null; recovery.atomic(file, fresh); await resume();
      log('system default Home enabled');
    } catch (e) {
      if (s) {
        try { await restore(s); } catch (cleanup) { throw new Error(e.message + '; independent recovery remains active: ' + cleanup.message); }
      }
      throw e;
    } finally { busy = false; }
  }
  async function startup() {
    if (recoveryRun) await recoveryRun;
    if (readConfig().startupEnabled !== true || busy) return { launched: false };
    busy = true;
    try {
      const marker = path.join(options.runtimeDir || '/tmp', 'homeplus-startup-requested');
      try { fs.closeSync(fs.openSync(marker, 'wx', 0o600)); } catch (e) { if (e.code === 'EEXIST') return { launched: false }; throw e; }
      const end = Date.now() + 20000;
      do {
        const r = await luna(MANAGER + 'getForegroundAppInfo', {});
        const id = r && (r.appId || (r.foregroundAppInfo && r.foregroundAppInfo[0] && r.foregroundAppInfo[0].appId));
        if (id === APP) return { launched: false };
        if (id && (id === 'com.webos.app.home' || /^com\.webos\.app\.(livetv|hdmi[0-9]+)$/.exec(id))) {
          await call(MANAGER + 'launch', { id: APP, params: { source: 'homeplus-boot' } });
          await foreground(APP); return { launched: true };
        }
        if (id) return { launched: false }; // somebody opened an app while the TV was starting
        await wait(250);
      } while (Date.now() < end);
      throw new Error('The TV did not finish choosing its startup screen');
    } catch (e) {
      if (readConfig().defaultHomeEnabled) await disable();
      throw e;
    } finally { busy = false; }
  }
  async function remove() {
    if (busy) throw new Error('A Home change is already in progress');
    busy = true;
    try {
      await disable();
      for (const name of ['state.json', 'state.json.ready', 'state.json.log', 'guard.js', 'boot.sh']) {
        try { fs.unlinkSync(path.join(stateDir, name)); } catch (e) { if (e.code !== 'ENOENT') throw e; }
      }
      try { fs.rmdirSync(stateDir); } catch (e) { if (!['ENOENT', 'ENOTEMPTY'].includes(e.code)) throw e; }
    } finally { busy = false; }
  }
  async function recoverNow() {
    if (busy || !isRoot()) return;
    const s = state();
    if (!s || !s.active) return;
    busy = true;
    try {
      if (s.deadline !== null || !readConfig().defaultHomeEnabled) await restore(s);
      else await startGuard(s);
    } finally { busy = false; }
  }
  function recover() {
    if (!recoveryRun) recoveryRun = recoverNow().then(
      (value) => { recoveryRun = null; return value; },
      (error) => { recoveryRun = null; throw error; });
    return recoveryRun;
  }
  return { status, setEnabled, startup, remove, recover };
};
