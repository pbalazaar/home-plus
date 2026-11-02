/* Independent Home recovery. Its installed copy lives outside the app and survives removal. */
'use strict';
const fs = require('fs');
const path = require('path');
const { execFile, spawn } = require('child_process');
const APP = 'com.github.pbalazaar.homeplus';
const MANAGER = 'luna://com.webos.applicationManager/';
const SETTINGS = 'luna://com.webos.settingsservice/';
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const validId = (s) => typeof s === 'string' && !!/^[a-zA-Z0-9._-]+$/.exec(s);

async function readDefaults(luna) {
  const r = await luna(SETTINGS + 'getSystemSettings', { category: 'general', keys: ['defaultApps'] });
  const apps = r && r.settings && r.settings.defaultApps;
  if (!r || r.returnValue !== true || !apps || typeof apps !== 'object' || Array.isArray(apps) ||
      (own(apps, 'home') && !validId(apps.home))) throw new Error('Cannot read the default Home setting');
  return Object.assign({}, apps);
}
async function accepted(luna, uri, params) {
  const r = await luna(uri, params);
  if (!r || r.returnValue !== true) throw new Error((r && r.errorText) || 'Home request refused');
  return r;
}
async function restore(luna, before, stockHome) {
  if (!validId(stockHome)) throw new Error('Invalid original Home app');
  let current = await readDefaults(luna);
  if (own(current, 'home') === own(before, 'home') && current.home === before.home) return { restored: true };
  if (current.home !== APP && !(current.home === stockHome && !own(before, 'home'))) return { preserved: true };
  if (own(before, 'home')) {
    await accepted(luna, MANAGER + 'setDefaultApp', { category: 'home', appId: before.home });
    if ((await readDefaults(luna)).home !== before.home) throw new Error('Previous Home was not restored');
  } else {
    if (current.home === APP) await accepted(luna, MANAGER + 'setDefaultApp', { category: 'home', appId: stockHome });
    current = await readDefaults(luna);
    if (!own(current, 'home')) return { restored: true };
    if (current.home !== stockHome) throw new Error('Home changed during recovery');
    delete current.home;
    await accepted(luna, SETTINGS + 'setSystemSettings', { category: 'general', settings: { defaultApps: current } });
    if (own(await readDefaults(luna), 'home')) throw new Error('Trial Home entry was not removed');
  }
  return { restored: true };
}
function syncDir(dir) {
  if (process.platform !== 'linux') return;
  const fd = fs.openSync(dir, 'r');
  try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
}
function durable(file, data, mode) {
  const tmp = file + '.' + process.pid + '.tmp';
  try { fs.unlinkSync(tmp); } catch (e) { if (e.code !== 'ENOENT') throw e; }
  const fd = fs.openSync(tmp, 'wx', mode || 0o600);
  try { fs.writeFileSync(fd, data); fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
  fs.renameSync(tmp, file);
  syncDir(path.dirname(file));
}
const atomic = (file, value) => durable(file, JSON.stringify(value, null, 2));
function load(file) {
  const s = JSON.parse(fs.readFileSync(file, 'utf8'));
  if (s.version !== 1 || typeof s.active !== 'boolean' || !(s.deadline === null || Number.isFinite(s.deadline)) ||
      !validId(s.stockHome) || !s.before || typeof s.before !== 'object' || Array.isArray(s.before) ||
      (own(s.before, 'home') && !validId(s.before.home)) || typeof s.applicationDir !== 'string' || !path.isAbsolute(s.applicationDir) ||
      typeof s.configPath !== 'string' || !path.isAbsolute(s.configPath) || !s.previous || typeof s.previous !== 'object' || Array.isArray(s.previous)) {
    throw new Error('Invalid default Home recovery state');
  }
  return s;
}
function restoreConfig(s) {
  let cfg;
  try { cfg = JSON.parse(fs.readFileSync(s.configPath, 'utf8')); } catch (e) { if (e.code === 'ENOENT') return; throw e; }
  if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) throw new Error('Invalid Home+ config');
  const owned = cfg.defaultHomeEnabled === true || s.deadline !== null;
  if (own(s.previous, 'defaultHomeEnabled')) cfg.defaultHomeEnabled = s.previous.defaultHomeEnabled;
  else delete cfg.defaultHomeEnabled;
  if (owned && (cfg.homeMode === 'watch' || cfg.homeOverride === false)) {
    for (const key of ['homeMode', 'homeOverride']) {
      if (own(s.previous, key)) cfg[key] = s.previous[key]; else delete cfg[key];
    }
  }
  atomic(s.configPath, cfg);
}
// A PTY keeps the reply on TV builds where luna-send otherwise exits without JSON.
function luna(uri, params) {
  const quote = (s) => "'" + s.replace(/'/g, "'\\''") + "'";
  const command = 'exec ' + quote(process.env.HOMEPLUS_RECOVERY_LUNA || 'luna-send') +
    ' -n 1 -w 4000 ' + quote(uri) + ' ' + quote(JSON.stringify(params));
  return new Promise((resolve, reject) => {
    const child = execFile('script', ['-q', '-c', command, '/dev/null'], { timeout: 8000 }, (err, stdout) => {
      if (err) return reject(new Error('Home recovery call failed: ' + err.message));
      let result;
      for (const line of String(stdout).split('\n')) {
        try { const r = JSON.parse(line.trim()); if (r && typeof r === 'object') result = r; } catch (e) { /* PTY output */ }
      }
      if (!result) return reject(new Error('Home recovery returned no JSON'));
      resolve(result);
    });
    child.stdin.end();
  });
}
async function main(mode, file) {
  if (mode === '--start' && file) {
    const state = load(file);
    const out = fs.openSync(file + '.log', 'a', 0o600);
    const child = spawn(process.execPath, [__filename, state.deadline === null ? '--watch' : '--restore', file], { detached: true, stdio: ['ignore', out, out] });
    child.unref(); fs.closeSync(out);
    console.log(JSON.stringify({ pid: child.pid }));
    return;
  }
  if (!['--watch', '--restore'].includes(mode) || !file) throw new Error('usage: default-home-guard.js --start|--watch|--restore state.json');
  let s = load(file), missingAt = 0, delay = 500;
  const transaction = s.transaction;
  const ownedLuna = (uri, params) => {
    // Defaults reads await the bus: another recovery and activation may finish before they return.
    const current = load(file);
    if (!current.active || current.transaction !== transaction) throw new Error('Home recovery no longer owns this transaction');
    return luna(uri, params);
  };
  if (!s.active) return;
  if (mode === '--watch') atomic(file + '.ready', { pid: process.pid, transaction: s.transaction });
  for (;;) {
    s = load(file);
    if (!s.active || s.transaction !== transaction) return;
    let available = false;
    try {
      const info = JSON.parse(fs.readFileSync(path.join(s.applicationDir, 'appinfo.json'), 'utf8'));
      available = info.id === APP && Array.isArray(info.appCategory) && info.appCategory.indexOf('home') !== -1 &&
        fs.existsSync(path.join(s.applicationDir, 'homeplus'));
    } catch (e) { /* removed, changing during an update, or no longer a Home package */ }
    if (available) missingAt = 0; else if (!missingAt) missingAt = Date.now();
    const expired = s.deadline !== null && Date.now() >= s.deadline;
    if (mode === '--watch' && !expired && (available || Date.now() - missingAt < 5000)) {
      await new Promise((resolve) => setTimeout(resolve, s.deadline === null ? 2000 : 250));
      continue;
    }
    try {
      await accepted(luna, MANAGER + 'getAppInfo', { id: s.stockHome });
      s = load(file);
      if (!s.active || s.transaction !== transaction) return;
      const result = await restore(ownedLuna, s.before, s.stockHome);
      s = load(file);
      if (!s.active || s.transaction !== transaction) return;
      restoreConfig(s);
      s.active = false; s.result = result;
      atomic(file, s);
      return;
    } catch (e) {
      await new Promise((resolve) => setTimeout(resolve, delay)); delay = Math.min(5000, delay * 2);
    }
  }
}
module.exports = { readDefaults, restore, restoreConfig, accepted, atomic, durable, syncDir, load, luna };
if (require.main === module) main(process.argv[2], process.argv[3]).catch((e) => { console.error(e.message); process.exitCode = 1; });
