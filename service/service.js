/* Home+ helper service.
 *
 * Runs on the TV as a webOS JS service. Once elevated (see README) it can call
 * the application manager on the launcher's behalf, and it talks to the Stremio
 * API so the launcher never holds your Stremio auth key. The native launcher
 * (com.github.pbalazaar.homeplus) uses its local HTTP API; Luna methods are for
 * terminal setup (Trakt, Stremio, Home button) and the boot script.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const https = require('https');
const http = require('http');
const dns = require('dns');
const pkg = require('./package.json');
const { requestJSON, onThisTV, requestOptions } = require('./net');
const Service = require('webos-service');
const callers = require('./callers');
const png = require('./png');
const tmp = require('./tmp');
const remove = require('./remove');
const { mkdirSync } = require('./mkdir');
const { sortStable } = require('./sort');
const { URL } = require('url');

const service = new Service(pkg.name);

// A bad request or a failed fetch must never take the root service (and with it
// the Home button) down: log it and keep serving.
process.on('uncaughtException', (e) => log('uncaught:', (e && e.stack) || e));
process.on('unhandledRejection', (e) => log('unhandled rejection:', (e && e.stack) || e));

// Diagnostics: /tmp/homeplus-service.log (cleared on each service start, capped at 256 KB; only ever
// the service's own file: service/tmp.js). HOMEPLUS_LOG: tests keep theirs elsewhere.
const LOG = process.env.HOMEPLUS_LOG || '/tmp/homeplus-service.log';
try { tmp.write(LOG, ''); } catch (e) { /* ignore */ }
function log(...parts) {
  const line = new Date().toISOString() + ' ' + parts.map((p) => (typeof p === 'string' ? p : JSON.stringify(p))).join(' ') + '\n';
  try { tmp.append(LOG, line, 256 * 1024); } catch (e) { /* ignore */ }
}
log('start', { version: pkg.version, uid: process.getuid ? process.getuid() : null, node: process.version });
const APPMGR = 'luna://com.webos.applicationManager';
const STREMIO_API = 'https://api.strem.io/api/';
const CACHE_MS = 60 * 1000;
// Local image server. webOS won't let a developer-installed web app read other
// apps' files, so the (root) service hands icons to the page over localhost.
const IMAGE_PORT = Number(process.env.HOMEPLUS_PORT) || 47823; /* tests run their own copy on another port */
const MIME = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp', '.gif': 'image/gif', '.svg': 'image/svg+xml' };

// ------------------------------------------------------------------ helpers

// A luna call's answer. One that never comes (a stuck service, a busy bus) mustn't keep a request
// waiting for good: after 15 s the call answers itself as the TV answers a refusal, which every
// caller handles (tests: HOMEPLUS_LUNA_TIMEOUT_MS).
const LUNA_TIMEOUT_MS = Number(process.env.HOMEPLUS_LUNA_TIMEOUT_MS) || 15000;
function luna(uri, params) {
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      log('luna: no answer from', uri);
      resolve({ returnValue: false, errorText: `no answer from ${uri}` });
    }, LUNA_TIMEOUT_MS);
    service.call(uri, params || {}, (m) => {
      clearTimeout(timer);
      resolve(m.payload);
    });
  });
}

function fail(msg, text) {
  msg.respond({ returnValue: false, errorText: String(text) });
}

// A luna method that only the TV's own tools and Home+ may call (service/callers.js)
function register(method, handler) {
  service.register(method, (msg) => {
    if (!callers.trusted(msg.sender)) {
      log('luna: refused', method, 'from', String(msg.sender || '?').slice(0, 80));
      return fail(msg, 'not permitted: Home+ answers only luna-send and Home+');
    }
    return handler(msg);
  });
}

// Config lives outside the app folder so reinstalling Home+ keeps you signed in.
// HOMEPLUS_CONFIG_DIR: tests and the desktop preview keep their config elsewhere (no root needed)
const CONFIG_DIRS = [process.env.HOMEPLUS_CONFIG_DIR, '/var/lib/homeplus', path.join(__dirname, 'data')].filter(Boolean);
let configPath = null;

function findConfigPath() {
  if (configPath) return configPath;
  for (const dir of CONFIG_DIRS) {
    try {
      mkdirSync(dir);
      fs.accessSync(dir, fs.constants.W_OK);
      configPath = path.join(dir, 'config.json');
      return configPath;
    } catch (e) { /* try the next one */ }
  }
  throw new Error('No writable config directory');
}

function readConfig() {
  try { return JSON.parse(fs.readFileSync(findConfigPath(), 'utf8')); } catch (e) { return {}; }
}

// Written whole or not at all: a power cut mid-write can't leave a truncated file behind (the new
// one is written beside it, flushed to the disk, then takes its place). mode: the new file's.
function writeFileAtomic(p, data, mode) {
  const beside = `${p}.${process.pid}.tmp`;
  try {
    const fd = fs.openSync(beside, 'w', mode);
    try {
      fs.writeFileSync(fd, data);
      fs.fsyncSync(fd);
    } finally {
      fs.closeSync(fd);
    }
    fs.renameSync(beside, p);
  } catch (e) {
    try { fs.unlinkSync(beside); } catch (e2) { /* never made */ }
    throw e;
  }
}

function writeConfig(cfg) {
  writeFileAtomic(findConfigPath(), JSON.stringify(cfg, null, 2), 0o600); /* sign-ins: root's only */
}

function postJSON(url, body) {
  return new Promise((resolve, reject) => {
    const data = JSON.stringify(body);
    const req = https.request(requestOptions(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) },
      timeout: 15000
    }), (res) => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { buf += c; });
      res.on('end', () => {
        if (res.complete === false) return; /* cut short: Node 12 ends it all the same ('close' rejects) */
        try { resolve(JSON.parse(buf)); } catch (e) { reject(new Error('Bad response from ' + url)); }
      });
      // The server hung up halfway through the reply: Node 16 and newer never end it, only close it
      res.on('close', () => { if (res.complete === false) reject(new Error('Connection closed before the reply was complete: ' + url)); });
    });
    req.on('timeout', () => req.destroy(new Error('Request timed out')));
    req.on('error', reject);
    req.end(data);
  });
}

async function stremio(method, body) {
  const r = await postJSON(STREMIO_API + method, body);
  if (r.error) throw new Error(r.error.message || JSON.stringify(r.error));
  return r.result;
}

// Stop webOS from reaping the service when it's idle: the image server has to stay up.
try {
  service.activityManager.create('keepAlive', () => {});
} catch (e) {
  log('keepAlive activity failed', String(e));
}

// ------------------------------------------------------------------ apps

// id -> { icon: [candidate paths], art: [candidate paths] }. Only paths the
// application manager reported are ever served.
let images = {};
let titles = {}; // app id -> title, for /api/featured

// Only an app's background image is artwork, and not LG's generic ones (square patterns in a
// colour): such apps get the glow made from their icon. Splash screens and Recents images are
// the app's logo, large.
const GENERIC_ART = /default_background|_preview\.(png|jpe?g)$/i;

function resolvePath(p, folder) {
  if (!p) return null;
  return p.startsWith('/') ? p : path.join(folder || '/', p);
}

let loggedSample = false;

function indexApps(apps) {
  const next = {};
  if (!loggedSample && apps && apps.length) {
    loggedSample = true;
    log('listApps', apps.length, 'apps; sample:', apps.slice(0, 3).map((a) => ({
      id: a.id, folderPath: a.folderPath, icon: a.icon, largeIcon: a.largeIcon, mediumIcon: a.mediumIcon, bgImage: a.bgImage
    })));
  }
  for (const a of apps || []) {
    const f = a.folderPath;
    next[a.id] = {
      icon: [a.extraLargeIcon, a.mediumLargeIcon, a.largeIcon, a.icon, a.mediumIcon].map((p) => resolvePath(p, f)).filter(Boolean),
      folder: f,
      art: [a.bgImage].map((p) => resolvePath(p, f)).filter((p) => p && !GENERIC_ART.test(path.basename(p)))
    };
  }
  images = next;
  titles = {};
  for (const a of apps || []) titles[a.id] = a.title || a.id;
}

async function listApps() {
  const r = await luna(APPMGR + '/listApps', {});
  if (r && r.apps) indexApps(r.apps);
  return r;
}

// An image file inside the app's own folder, symlinks resolved (a hostile app
// can't point its icon at /var/lib/homeplus/config.json); else null.
function insideApp(file, folder) {
  try {
    const real = fs.realpathSync(file), dir = fs.realpathSync(folder);
    return real.startsWith(dir + path.sep) && MIME[path.extname(real).toLowerCase()] && fs.statSync(real).isFile() ? real : null;
  } catch (e) {
    return null;
  }
}

function firstExisting(files, folder) {
  for (const f of files || []) {
    const ok = insideApp(f, folder);
    if (ok) return ok;
  }
  return null;
}

// When the reported files are missing, look for a likely icon in the app's folder.
function guessIcon(folder) {
  try {
    const names = fs.readdirSync(folder).filter((n) => /icon.*\.(png|jpe?g|webp)$/i.test(n));
    sortStable(names, (a, b) => (/large|big|130|256|512/i.test(b) ? 1 : 0) - (/large|big|130|256|512/i.test(a) ? 1 : 0));
    return names.length ? insideApp(path.join(folder, names[0]), folder) : null;
  } catch (e) {
    return null;
  }
}

async function imageFile(kind, id) {
  if (!images[id]) await listApps();
  const entry = images[id];
  if (!entry) { log('image: unknown app', id); return null; }
  if (!entry.folder) return null;
  let file = firstExisting(entry[kind], entry.folder);
  if (!file && kind === 'icon') file = guessIcon(entry.folder);
  if (!file && kind === 'icon') log('image: no icon file for', id, entry[kind], 'folder', entry.folder);
  return file;
}

// ------------------------------------------------------------------ Home button takeover
// Watches the foreground app; when LG's own home screen comes up (the Home
// button), launches Home+ instead. Settings live in config.json:
//   homeOverride: true/false (default true)
//   homeApps:     app ids that count as "LG home" (default below)
//   homeTarget:   app to open (default: Home+)

const DEFAULT_HOME_APPS = ['com.webos.app.home'];
const NATIVE_ID = 'com.github.pbalazaar.homeplus'; // the launcher; this service ships in its package
// Home+'s id up to 0.14.0. A new id is another app to webOS, installed beside the old one until that's
// uninstalled: a bind LG Input Hook's app made for it is still Home+'s, the old launcher stays out of
// the app list, and Remove Home+ takes what it left too.
const OLD_ID = 'com.github.pmiraka.homeplus';
let lastForeground = '';
let resumeRun = null; // the title OK last asked to play: { app, seen }, seen once its app has been in front
let lastTakeover = 0;
let takeoverLanded = true; // did the launcher reach the front after our last takeover?

function homeTarget(cfg) {
  return cfg.homeTarget || NATIVE_ID;
}

// ---- no-flash mode (user-approved): replace LG home's start page while the service runs.
// The page LG's home app loads is bind-mounted (in memory only; nothing on disk
// changes) with a black page that immediately opens Home+. Rebooting without the
// service, or setHomeOverride {"mode":"watch"} / {"enabled":false}, restores LG's home.

const LG_HOME = 'com.webos.app.home';
const { execFile } = require('child_process');
let redirectTarget = null; // path we mounted over, if any

function redirectPage(target) {
  const ids = JSON.stringify([target]).replace(/</g, '\\u003c'); // no "</script>" from an id
  return `<!doctype html><html><head><meta charset="utf-8">
<style>html,body{margin:0;width:100%;height:100%;background:#000}</style></head><body><script>
// Home+ no-flash page: LG's home app now just opens Home+ (see the Home+ service).
var ids = ${ids}, bridges = [];
function open(i) {
  if (i >= ids.length) return;
  var b = new PalmServiceBridge();
  bridges.push(b);
  b.onservicecallback = function (m) {
    try { if (JSON.parse(m).returnValue === false) open(i + 1); } catch (e) { open(i + 1); }
  };
  b.call('luna://com.webos.applicationManager/launch', JSON.stringify({ id: ids[i] }));
}
open(0);
document.addEventListener('webOSRelaunch', function () { open(0); });
document.addEventListener('visibilitychange', function () { if (!document.hidden) open(0); });
</script></body></html>`;
}

function run(cmd, args) {
  return new Promise((resolve) => execFile(cmd, args, (err, stdout, stderr) => resolve({ err, out: String(stdout || stderr || '') })));
}

async function lgHomeEntry() {
  const r = await luna(APPMGR + '/listApps', {});
  const home = ((r && r.apps) || []).find((a) => a.id === LG_HOME);
  const folder = (home && home.folderPath) || `/usr/palm/applications/${LG_HOME}`;
  let info;
  try { info = JSON.parse(fs.readFileSync(path.join(folder, 'appinfo.json'), 'utf8')); } catch (e) { return { error: 'no appinfo in ' + folder }; }
  if (info.type && info.type !== 'web') return { error: `LG home is a ${info.type} app` };
  const main = path.join(folder, info.main || 'index.html');
  if (!fs.existsSync(main)) return { error: 'missing ' + main };
  return { main };
}

// LG's home app doesn't change while we run, so check once whether its page can be redirected.
let lgHomeChecked = null;
async function redirectAvailable() {
  if (!lgHomeChecked) lgHomeChecked = await lgHomeEntry();
  return !lgHomeChecked.error;
}

// No Flash: LG's home page redirected or, where it can't be (the C3's home is a native app), Home+'s
// key filter taking the Home key. Not offered while LG Input Hook's Home bind (Home+'s) takes it.
async function noFlashAvailable() {
  return ((await redirectAvailable()) || keyFilter.available()) && !hookHome();
}

async function mountRedirect() {
  if (redirectTarget) return true;
  const entry = await lgHomeEntry();
  if (entry.error) { log('no-flash mode unavailable:', entry.error); return false; }
  const src = path.join(path.dirname(findConfigPath()), 'home-redirect.html');
  fs.writeFileSync(src, redirectPage(homeTarget(readConfig())));
  const mounts = fs.readFileSync('/proc/mounts', 'utf8');
  if (!mounts.includes(' ' + entry.main + ' ')) {
    const r = await run('mount', ['--bind', src, entry.main]);
    if (r.err) { log('no-flash mount failed:', r.out.trim() || r.err.message); return false; }
  }
  redirectTarget = entry.main;
  // restart LG's home so its next start loads the no-flash page
  await luna(APPMGR + '/closeByAppId', { id: LG_HOME });
  log('no-flash mode on:', entry.main);
  return true;
}

async function unmountRedirect() {
  if (!redirectTarget) return;
  const r = await run('umount', [redirectTarget]);
  log('no-flash mode off', r.err ? r.out.trim() : '');
  redirectTarget = null;
  await luna(APPMGR + '/closeByAppId', { id: LG_HOME });
}

// Until the launcher has asked "Use Home+ as your home screen?", the Home button is left alone.
function homeMode(cfg) {
  if (cfg.homeOverride === false || (!cfg.homeAsked && !cfg.homeMode)) return 'off';
  return cfg.homeMode || 'watch';
}

// The launcher's welcome steps are done (or skipped). Home+ before them asked only about the Home button: answered
// there, it counts as done, so an update doesn't welcome you again.
function onboarded(cfg) {
  return typeof cfg.onboarded === 'boolean' ? cfg.onboarded : !!cfg.homeAsked;
}

async function applyHomeMode() {
  if (homeMode(readConfig()) === 'redirect' && (await redirectAvailable())) {
    if (!(await mountRedirect())) log('no-flash unavailable; using watch mode');
  } else {
    await unmountRedirect();
  }
  await applyKeyFilter(); /* (No Flash without the redirect: the key filter's) */
}
setTimeout(() => applyHomeMode().catch((e) => log('home mode error', e.message)), 1500);

// ---- boot script: once elevated, the service links the one in its package into Homebrew
// Channel's init.d, so Homebrew Channel re-elevates and starts it at every boot. A link, not a
// copy (Homebrew Channel's rules): it goes with the package, and run-parts skips the dead link.

const APP_DIR = process.env.HOMEPLUS_APP_DIR || path.join(__dirname, '..', '..', 'applications', NATIVE_ID); /* tests: elsewhere */
const INITD_DIR = '/var/lib/webosbrew/init.d';
const INITD = path.join(process.env.HOMEPLUS_INITD_DIR || INITD_DIR, '50-homeplus'); /* tests: elsewhere */
const BOOT_LOG = path.join(path.dirname(INITD), '..', 'homeplus-boot.log'); /* the boot script's */

function installBootScript() {
  if (!isRoot()) return false;
  const script = path.join(APP_DIR, 'init.d', '50-homeplus');
  try {
    let have = null;
    try { have = fs.readlinkSync(INITD); } catch (e) { /* not there yet, or a copy */ }
    if (have === script) return true;
    if (!fs.existsSync(script)) { log('boot script: no', script); return false; }
    mkdirSync(path.dirname(INITD)); /* some ways of rooting never made it */
    remove.rmSync(INITD, { force: true }); /* the copy Home+ 0.11 and older made */
    fs.symlinkSync(script, INITD);
    log('boot script linked:', INITD, '->', script);
    return true;
  } catch (e) {
    log('boot script failed:', e.message);
    return false;
  }
}

function isRoot() { return !!(process.getuid && process.getuid() === 0); }
setTimeout(installBootScript, 2000);

// ---- Remove Home+: undo everything Home+ set up on the TV, then uninstall the package.

const INPUTHOOK_BINDS = process.env.HOMEPLUS_INPUTHOOK_BINDS || '/home/root/.config/lginputhook/keybinds.json'; /* tests: elsewhere */
const OUR_IDS = [NATIVE_ID, OLD_ID, 'com.atvlauncher.native', 'com.atvlauncher.app'];

// a key's bind that's Home+'s: the one Home+ makes there (OUR_BINDS), or one it turns into that
// (OLD_BINDS). Matched whole: a bind of yours that only mentions Home+ stays yours.
const ourBind = (b, key) => !!b && !!OUR_BINDS[key] &&
  ((b.action === OUR_BINDS[key].action && b.command === OUR_BINDS[key].command) || OLD_BINDS[key](b));

function dropInputhookBinds() {
  // lginputhook keybinds that are Home+'s are removed; other binds stay
  const binds = readBinds();
  if (!binds) throw new Error('keybinds.json isn\'t valid JSON: its binds were left alone');
  let n = 0;
  for (const [key, b] of Object.entries(binds)) {
    if (ourBind(b, key)) { delete binds[key]; n++; }
  }
  if (n) writeBinds(binds);
  return n;
}

// ---- The remote's Settings key (LG Input Hook's code 139): Home+'s Control Center while Home+
// is in front, else LG's Quick Settings. The hook runs settings-key.sh, which asks us.

const SETTINGS_KEY = '139';
const SETTINGS_KEY_SCRIPT = path.join(__dirname, 'settings-key.sh');

// ---- The remote's Input key (LG Input Hook's code 241, KEY_VIDEO_NEXT): the Control Center's inputs,
// in Home+ or over whatever's on screen, else LG's Input picker. The hook runs input-key.sh, which asks us.

const INPUT_KEY = '241';
const INPUT_KEY_SCRIPT = path.join(__dirname, 'input-key.sh');
const INPUT_PICKER = 'com.webos.app.quickinputpicker';

// ---- Without LG Input Hook: Home+'s key filter in LG's compositor takes the same keys (service/keyfilter.js).
// The Settings and Input keys are Home+'s when its bind in LG Input Hook is, or config.json says so
// (settingsKey, inputKey: Settings' buttons, kept there for the key filter).

const KEY_PREFS = { [SETTINGS_KEY]: 'settingsKey', [INPUT_KEY]: 'inputKey' };
const FILTER_KEYS = { [SETTINGS_KEY]: 'settings', [INPUT_KEY]: 'input' }; /* (the key filter's names for them) */
const keyFilter = require('./keyfilter')({
  luna,
  subscribe: (uri, params) => service.subscribe(uri, params),
  log,
  dir: () => path.dirname(findConfigPath()),
  write: (file, text) => writeFileAtomic(file, text, 0o600), /* (the API's token is in it; the compositor runs as root) */
  api: `http://127.0.0.1:${IMAGE_PORT}/api/key`,
  token: () => TOKEN,
  appId: NATIVE_ID,
  lgHome: LG_HOME
});
if (isRoot()) keyFilter.start();

function keyOn(key, cfg = readConfig()) {
  const binds = readBinds();
  return (!!binds && ourBind(binds[key], key)) || cfg[KEY_PREFS[key]] === true;
}

// LG Input Hook's Home bind is Home+'s: the hook takes the key itself, as before No Flash was offered here
function hookHome() {
  const binds = readBinds();
  return !!binds && ourBind(binds[HOME_KEY], HOME_KEY);
}

// The keys the key filter takes: Home with No Flash where LG's home page can't be redirected, and Settings
// and Input while their buttons are Home+'s. Each time one of those may have changed: true, or why the TV
// didn't take it.
async function applyKeyFilter() {
  const cfg = readConfig();
  return keyFilter.set({
    home: homeMode(cfg) === 'redirect' && !(await redirectAvailable()),
    settings: keyOn(SETTINGS_KEY, cfg),
    input: keyOn(INPUT_KEY, cfg)
  });
}

// LG Input Hook's binds ({} while it has none). null when the file can't be read or isn't JSON
// (its app may be writing it, or a comma's astray): then it's left as it is, never written over
// with only Home+'s binds in it
let bindsBad = ''; /* why, said once in the log */
function readBinds() {
  try {
    const binds = JSON.parse(fs.readFileSync(INPUTHOOK_BINDS, 'utf8'));
    if (!binds || typeof binds !== 'object' || Array.isArray(binds)) throw new Error('not an object of binds');
    bindsBad = '';
    return binds;
  } catch (e) {
    if (e.code === 'ENOENT') return {};
    if (bindsBad !== e.message) log('keybinds.json isn\'t valid JSON; leaving it alone:', e.message);
    bindsBad = e.message;
    return null;
  }
}

// LG Input Hook rereads the file whenever it changes, so it's written whole, as readable as it was
function writeBinds(binds) {
  let mode = 0o644;
  try { mode = fs.statSync(INPUTHOOK_BINDS).mode & 0o777; } catch (e) { /* a new file */ }
  writeFileAtomic(INPUTHOOK_BINDS, JSON.stringify(binds, null, 2), mode);
}

// ---- Home+'s binds outlive it when it's removed without Remove Home+ (from the Homebrew Channel,
// say). So each runs a small script beside LG Input Hook's binds, which outlives Home+ as they do:
// it checks that Home+ is still installed, by a file of its package (its folders can outlive it),
// and otherwise does what the key does without Home+. The binds themselves are plain commands: LG
// Input Hook runs one without a shell (on the C3, a bind starting with `[` or `{` did nothing).

const HOME_KEY = '773';
const HOME_SCRIPT = path.join(path.dirname(INPUTHOOK_BINDS), 'homeplus-home.sh');
const SETTINGS_SCRIPT = path.join(path.dirname(INPUTHOOK_BINDS), 'homeplus-settings.sh');
const INPUT_SCRIPT = path.join(path.dirname(INPUTHOOK_BINDS), 'homeplus-input.sh');
const lunaSend = (uri, params) => `luna-send -n 1 ${uri} '${JSON.stringify(params)}'`;
const KEY_SCRIPTS = {
  [HOME_SCRIPT]: `#!/bin/sh
# Home+'s Home button, run by LG Input Hook: Home+, or LG's home screen once Home+ is gone.
[ -f ${path.join(APP_DIR, 'appinfo.json')} ] && exec ${lunaSend(APPMGR + '/launch', { id: NATIVE_ID })}
exec ${lunaSend(APPMGR + '/launch', { id: LG_HOME })}
`,
  [SETTINGS_SCRIPT]: `#!/bin/sh
# Home+'s Settings button, run by LG Input Hook: Home+'s Control Center, or LG's Quick Settings once Home+ is gone.
[ -f ${SETTINGS_KEY_SCRIPT} ] && exec sh ${SETTINGS_KEY_SCRIPT}
exec ${lunaSend('luna://com.webos.service.systemUIManager/launch', { appId: 'com.webos.app.quicksettings' })}
`,
  [INPUT_SCRIPT]: `#!/bin/sh
# Home+'s Input button, run by LG Input Hook: Home+'s inputs, or LG's Input picker once Home+ is gone.
[ -f ${INPUT_KEY_SCRIPT} ] && exec sh ${INPUT_KEY_SCRIPT}
exec ${lunaSend('luna://com.webos.service.systemUIManager/launch', { appId: INPUT_PICKER })}
`
};
const OUR_BINDS = { /* Home: Home+, else LG's home screen. Settings, Input: our script, else LG's Quick Settings, Input picker */
  [HOME_KEY]: { action: 'exec', command: `sh ${HOME_SCRIPT}` },
  [SETTINGS_KEY]: { action: 'exec', command: `sh ${SETTINGS_SCRIPT}` },
  [INPUT_KEY]: { action: 'exec', command: `sh ${INPUT_SCRIPT}` }
};

function writeKeyScripts() {
  for (const [file, text] of Object.entries(KEY_SCRIPTS)) {
    let had = null;
    try { had = fs.readFileSync(file, 'utf8'); } catch (e) { /* not there yet */ }
    if (had !== text) writeFileAtomic(file, text, 0o755); /* (a key's bind runs it: never half of one) */
  }
}

// the binds Home+ turns into its own: the Home key's "Launch App: Home+", which LG Input Hook's app
// makes (Home+'s older ids too), and the Settings key's from Home+ 0.11, which ran its script itself
const OLD_BINDS = {
  [HOME_KEY]: (b) => b.action === 'launch' && OUR_IDS.includes(b.id),
  [SETTINGS_KEY]: (b) => b.action === 'exec' && b.command === `sh ${SETTINGS_KEY_SCRIPT}`,
  [INPUT_KEY]: () => false
};

function upgradeBinds() {
  const binds = readBinds();
  if (!binds) return; /* not JSON (being written, say): left alone */
  if (Object.entries(binds).some(([k, b]) => ourBind(b, k))) writeKeyScripts(); /* (an update may change them) */
  const keys = Object.keys(OLD_BINDS).filter((k) => binds[k] && OLD_BINDS[k](binds[k]));
  if (!keys.length) return;
  for (const k of keys) binds[k] = OUR_BINDS[k];
  writeBinds(binds);
  log('LG Input Hook binds now work without Home+:', keys);
}

// at start, and whenever the binds change, so a Home bind just made in LG Input Hook's app is
// upgraded at once (the hook rereads the file by itself). LG Input Hook installed after the service
// started is looked for every minute (tests: HOMEPLUS_BINDS_RETRY_MS).
const BINDS_RETRY_MS = Number(process.env.HOMEPLUS_BINDS_RETRY_MS) || 60000;
if (isRoot()) {
  const upgrade = () => {
    try { upgradeBinds(); } catch (e) { log('LG Input Hook binds:', e.message); }
    applyKeyFilter().catch((e) => log('key filter:', e.message)); /* (a key's bind may have come or gone) */
  };
  let timer = null;
  const watch = () => {
    try {
      fs.watch(path.dirname(INPUTHOOK_BINDS), { persistent: false }, () => { clearTimeout(timer); timer = setTimeout(upgrade, 500); })
        .on('error', () => {});
    } catch (e) { /* no LG Input Hook (yet) */
      setTimeout(watch, BINDS_RETRY_MS).unref();
      return;
    }
    upgrade(); /* (binds it may have come with) */
  };
  watch();
}

// a key's button (the Settings or Input key's): LG Input Hook or Home+'s key filter is there to take the
// key, and it's Home+'s
function keyState(key) {
  const hook = fs.existsSync(path.dirname(INPUTHOOK_BINDS));
  /* without LG Input Hook, what the compositor's list has: a change configd didn't take isn't on */
  return { available: hook || keyFilter.available(), on: keyOn(key) && (hook || keyFilter.takes(FILTER_KEYS[key])) };
}
const settingsKeyState = () => keyState(SETTINGS_KEY);

// kept in cfg (written here); then applyKeyFilter()
function setKeyBind(key, on, cfg = readConfig()) {
  const hook = fs.existsSync(path.dirname(INPUTHOOK_BINDS));
  if (!hook && !keyFilter.available()) throw new Error('LG Input Hook isn\'t installed');
  if (hook) { /* its bind too: the hook takes the key before the compositor sees it */
    const binds = readBinds();
    if (!binds) throw new Error('LG Input Hook\'s keybinds.json isn\'t valid JSON'); /* (Settings says so; the file stays as it is) */
    if (on) {
      writeKeyScripts();
      binds[key] = OUR_BINDS[key];
    }
    else if (ourBind(binds[key], key)) delete binds[key]; /* someone else's bind stays */
    writeBinds(binds);
  }
  cfg[KEY_PREFS[key]] = on;
  writeConfig(cfg);
  log(key === INPUT_KEY ? 'input key:' : 'settings key:', on ? 'Home+' : 'LG');
}

// The Settings key: Home+'s Control Center; the Input key: its inputs, on their own. In front of Home+
// the launcher opens its own; over anything else it's a system UI that LG's compositor runs
// (native/qml/), as it runs LG's Quick Settings, so the video underneath keeps playing. LG's Quick
// Settings (or Input picker) if that can't open. The Input key again, while it's open, turns it to its
// inputs, or closes it if they're on show; it's asked of the open panel, which keeps its frames.
async function remoteKey(key) {
  if (key !== 'settings' && key !== 'input') throw new Error(`not a key Home+ handles: ${key}`);
  if (lastForeground === NATIVE_ID) {
    emit(key === 'input' ? 'inputs' : 'controlCenter');
    return { opened: 'controlCenter' };
  }
  if (key === 'input' && overlayOpen) {
    await luna(SYSUI + '/launch', { appId: OVERLAY_ID, main: overlayMain(), windowType: '_WEBOS_SYSTEM_UI_TYPE_OVERLAY',
      defaultWindowType: 'overlay', params: { again: 'input', port: IMAGE_PORT, token: TOKEN } });
    return { opened: 'overlay' };
  }
  try {
    return { opened: (await openOverlay(key === 'input' ? 'inputs' : '')) ? 'overlay' : 'nothing' }; /* nothing: another app came to the front */
  } catch (e) {
    log('control center overlay:', e.message);
  }
  if (key === 'input') {
    await inputPicker();
    return { opened: 'inputPicker' };
  }
  await quickSettings();
  return { opened: 'quickSettings' };
}

const quickSettings = () => luna(SYSUI + '/launch', { appId: 'com.webos.app.quicksettings' });
const inputPicker = () => luna(SYSUI + '/launch', { appId: INPUT_PICKER });

// ---- The Control Center over other apps. A second launch (the Settings key again) closes it;
// so does another app coming to the front.

const SYSUI = 'luna://com.webos.service.systemUIManager';
const OVERLAY_ID = 'com.github.pbalazaar.homeplus.cc';
const OVERLAY_QML = path.join(APP_DIR, 'qml', 'ControlCenter.qml');
let overlayOpen = false;
let overlayGen = 0; /* counts each open, and each thing that cancels one on its way (another app in front) */
let overlayBusy = false; /* an open on its way */
let overlayAsked = 0; /* when it was asked for: how long it takes to show is logged */
let overlayInputs = []; /* the inputs it offered: the only apps it may open */
let overlayList = ''; /* opened on a list of its own ('inputs': the Input key): if it fails, LG's picker for it */

// false: cancelled on the way (another app came to the front). list: 'inputs' opens it on them alone
async function openOverlay(list = '') {
  if (!fs.existsSync(OVERLAY_QML)) throw new Error('not installed');
  const gen = ++overlayGen;
  overlayBusy = true;
  overlayAsked = Date.now();
  overlayList = list;
  let frames = '', asked = false, opened = false, over = false; /* its frames' folder; the panel asked for; on screen; not to be */
  const live = () => gen === overlayGen && !over && (!opened || overlayOpen); /* (not closed, nor opened again) */
  try {
    const prefs = readConfig().prefs || {}; /* the launcher's: its clock, Settings -> Control Center and Accessibility */
    frames = prefs.reduceTransparency ? '' : framesDir(); /* solid panels: no frames */
    framePicture = false;
    // what's on screen now (DISPLAY: the app's own screen too), before the panel is drawn over it:
    // the glass's first picture, and the one it keeps over an app with no video. The TV takes it as
    // the call starts (seen on the C3), so it isn't waited for: the panel isn't on screen yet. If the
    // TV is still taking another (the last opening's) when the panel is asked for, there's none. It
    // isn't held back for the rate as frames are: held, it would often be none, and over an app with no
    // video, glass with no picture while it's open; the TV takes one capture at a time, ~350 ms each.
    const snapshot = frames && grab(frames, 'DISPLAY', () => live() && !asked, live);
    const state = await control.state();
    if (gen !== overlayGen) return false;
    overlayInputs = (state.inputs || []).map((i) => i.appId);
    Object.assign(state, { clock24: !!prefs.clock24, reduceMotion: !!prefs.reduceMotion, reduceTransparency: !!prefs.reduceTransparency,
      sounds: prefs.sounds !== false });
    if (fs.existsSync('/tmp/homeplus-cc-fps')) state.debugFps = true; /* its frame rate, on screen */
    asked = true;
    const r = await luna(SYSUI + '/launch', {
      appId: OVERLAY_ID, main: overlayMain(), windowType: '_WEBOS_SYSTEM_UI_TYPE_OVERLAY', defaultWindowType: 'overlay',
      params: { state, port: IMAGE_PORT, token: TOKEN, frame: frames && 'file://' + path.join(frames, 'frame.png'), list } /* the API: the compositor may only call LG's services over luna */
    });
    if (!r || r.returnValue === false) throw new Error((r && r.errorText) || "the TV didn't open it");
    if (gen !== overlayGen) { /* another app came to the front while it opened */
      luna(SYSUI + '/closeByAppId', { appId: OVERLAY_ID });
      return false;
    }
    overlayOpen = opened = true;
    if (frames) framesLoop = framesLoop.then(() => captureFrames(gen, frames, snapshot, !!prefs.ccBlurVideo));
    return true;
  } finally {
    overlayBusy = false;
    over = !opened;
    if (frames && over) dropFrames(frames); /* (cancelled, refused, or a mistake) */
  }
}

// Its glass shows the picture behind it, blurred: first a snapshot of the screen as it opens, then,
// while it's open, a small frame of the video (VIDEO: without any graphics, so not the Control Center
// itself) about three times a second. With no frame, the glass is clear. The TV can't always
// capture its video: not protected video, and not while its one video capture is taken (PicCap's,
// for the ambient light, takes it; Blur Over Video pauses PicCap's while it's open): no frames of
// the video then, and the screen's are black where it is, so a frame that's black where the panel
// is isn't shown. The frames are what's
// on screen, so they go in a folder the service makes for each opening, with a name nobody can
// guess, that only root can read (the compositor, root, shows them); in memory, and deleted when it
// closes.
const FRAMES_IN = process.env.HOMEPLUS_FRAMES_DIR || os.tmpdir(); /* tests: elsewhere */
const framesDir = () => fs.mkdtempSync(path.join(FRAMES_IN, 'homeplus-cc-')); /* new, root's, 0700 */
try { /* left by a service that stopped (only its own) */
  for (const f of fs.readdirSync(FRAMES_IN).filter((f) => /^homeplus-cc-/.test(f))) {
    const p = path.join(FRAMES_IN, f), st = fs.lstatSync(p);
    if (st.isDirectory() && process.getuid && st.uid === process.getuid()) remove.rmSync(p, { recursive: true, force: true });
  }
} catch (e) { /* nothing there */ }
const settle = (promise, ms) => Promise.race([promise, new Promise((done) => setTimeout(done, ms, null))]);
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

const PANEL = { x0: 0.75, x1: 0.98, y0: 0.04, y1: 0.93 }; /* where the panel is (ControlCenterView.qml) */

// Each change of the glass's picture (a new frame, or none: the glass is clear) has a number. The
// panel asks for the next ({"event": "frame", "after": n}, POST /api/overlay), and the answer waits
// for it (or 2 s): a frame shows as soon as it's taken, not when the panel next looks for one.
let frameN = 0, framePicture = false;
const frameWaiters = new Set();
function frameChanged(picture) {
  frameN++;
  framePicture = picture;
  for (const wake of frameWaiters) wake();
}
function nextFrame(after) {
  return new Promise((resolve) => {
    let timer = null;
    const answer = () => {
      clearTimeout(timer);
      frameWaiters.delete(answer);
      resolve({ returnValue: true, n: frameN, picture: framePicture });
    };
    if (frameN > after) return answer();
    timer = setTimeout(answer, 2000);
    frameWaiters.add(answer);
  });
}

// The TV takes one capture at a time, whoever asks: an opening's snapshot, its frames of the video,
// the next opening's (a call it hasn't answered may still be taking its picture). One that hasn't
// answered in 3 s gets no frame, but the next still waits for its answer (luna gives up after 15 s),
// and what it writes then is deleted. Each has a file of its own, in its opening's folder; a folder
// goes once what's being taken into it has come.
const CAPTURE_TIMEOUT_MS = Number(process.env.HOMEPLUS_CAPTURE_TIMEOUT_MS) || 3000; /* tests: less */
let captureFree = Promise.resolve(); /* the last capture asked for has answered, or wasn't taken */
let captureAt = 0, captureN = 0; /* when the last one started; how many there have been */

// a capture into a file of its own in dir, once the TV is free, if go() still says so then: the TV's
// answer (null: none in time), and the file
async function capture(dir, method, go) {
  const file = path.join(dir, `capture-${++captureN}.png`), before = captureFree;
  let free;
  captureFree = new Promise((done) => { free = done; });
  await before;
  if (!go()) {
    free();
    return { file, r: { returnValue: false, errorText: 'not taken' } };
  }
  captureAt = Date.now();
  const answer = luna('luna://com.webos.service.capture/executeOneShot', { path: file, method, format: 'PNG', width: 160, height: 90 });
  answer.then(free, free);
  const r = await settle(answer, CAPTURE_TIMEOUT_MS);
  const drop = () => remove.rm(file, { force: true }, () => {});
  if (!r) answer.then(drop, drop); /* given up on: whatever it writes goes */
  return { file, r };
}
const dropFrames = (dir) => captureFree.then(() => remove.rm(dir, { recursive: true, force: true }, () => {}));

// a small picture of the screen (DISPLAY) or the video (VIDEO) into dir/frame.png, whole or not at
// all, taken if go() says so once the TV is free, and kept if live() still says so when it comes:
// 'picture'; 'same' as the one on show (not shown again: a still screen, a paused video); 'dark'
// (not kept) if it's black where the panel is; else why there's none
let shown = null; /* the frame on show: its folder and its bytes */
async function grab(dir, method, go, live) {
  let file = '';
  try {
    const c = await capture(dir, method, go);
    file = c.file;
    if (!c.r || !c.r.returnValue) return (c.r && c.r.errorText) || 'no answer';
    if (!live()) return 'too late'; /* (it closed, or opened again, meanwhile) */
    const bytes = fs.readFileSync(file);
    if (shown && shown.dir === dir && bytes.equals(shown.bytes)) return 'same';
    if (png.brightness(bytes, PANEL) < 24) return 'dark';
    fs.renameSync(file, path.join(dir, 'frame.png'));
    shown = { dir, bytes };
    frameChanged(true);
    return 'picture';
  } catch (e) {
    return e.message;
  } finally {
    if (file) remove.rmSync(file, { force: true }); /* (unless it's the frame now) */
  }
}

// One opening's frames at a time: the next one's start once the last one's have stopped (as soon as
// it closed, or its capture on the way has come), so PicCap, paused for one, stays paused for the next
// (ambient.js starts it again only a while after). At most three frames a second, counted from when
// the last capture started (on the C3 one takes ~350 ms, so this seldom waits); with no picture, soon
// again at first (the video was dark a moment, or PicCap is letting go of the capture), then less and
// less often.
let framesLoop = Promise.resolve();
const CAPTURE_MS = 1000 / (Number(process.env.HOMEPLUS_CAPTURE_RATE) || 3); /* tests: more */
const RETRY_MS = (process.env.HOMEPLUS_CAPTURE_RETRY_MS || '1500,3000,5000').split(',').map(Number); /* tests: shorter */

// waits ms, or less if live() says it has closed (looked at every 100 ms)
async function nap(ms, live) {
  for (const end = Date.now() + ms; live() && Date.now() < end;) await sleep(Math.min(end - Date.now(), 100));
}

async function captureFrames(gen, dir, snapshot, blurVideo) {
  const live = () => overlayOpen && gen === overlayGen;
  const until = Date.now() + 5 * 60000; /* in case it's never said to have closed */
  let video = false; /* the frame on show is the video's, not the snapshot */
  let paused = false; /* PicCap's capture, for Blur Over Video */
  let wait = 0, none = 0, slow = 0; /* before the next try; tries in a row with no picture, and long waits after them */
  try {
    const screen = await snapshot;
    if (blurVideo && picCapture && live()) paused = await picCapture.pause();
    for (let n = 0; ; n++) {
      await nap(Math.max(wait, captureAt + CAPTURE_MS - Date.now()), live);
      if (!live() || Date.now() > until) break;
      const got = await grab(dir, 'VIDEO', live, live); /* none (no video playing): the snapshot stays */
      if (!live()) break;
      if (!n) log('control center frames: the screen', screen, '- the video', got, paused ? '(PicCap paused)' : '');
      if (got === 'picture' || got === 'same') {
        video = true;
        wait = none = slow = 0;
        continue;
      }
      if (video) { /* the video went dark, or can't be captured now: clear glass, over what's there */
        remove.rmSync(path.join(dir, 'frame.png'), { force: true });
        shown = null;
        frameChanged(false);
        video = false;
      }
      none++;
      wait = (got === 'dark' && none <= 3) || (paused && n < 10) ? 300 : RETRY_MS[Math.min(slow++, RETRY_MS.length - 1)];
    }
  } catch (e) {
    log('control center frames:', e.message);
  } finally {
    if (paused) picCapture.resume();
    if (gen === overlayGen) frameChanged(false); /* (the panel's waiting asks are answered; a newer one's frames are its own) */
    if (shown && shown.dir === dir) shown = null;
    dropFrames(dir);
  }
}

// The compositor keeps every QML file it has loaded, by its address, until the TV restarts; so each
// version of the files gets addresses of its own: a link to the folder named for their time.
function overlayMain() {
  const dir = path.dirname(OVERLAY_QML);
  const t = Math.max(...fs.readdirSync(dir).filter((f) => f.endsWith('.qml')).map((f) => fs.statSync(path.join(dir, f)).mtimeMs));
  const name = `qml-${Math.floor(t).toString(36)}`;
  for (const f of fs.readdirSync(APP_DIR)) { /* older versions' links */
    if (/^qml-/.test(f) && f !== name && fs.lstatSync(path.join(APP_DIR, f)).isSymbolicLink()) fs.unlinkSync(path.join(APP_DIR, f));
  }
  try { fs.symlinkSync('qml', path.join(APP_DIR, name)); } catch (e) { if (e.code !== 'EEXIST') throw e; }
  return `file://${path.join(APP_DIR, name, 'ControlCenter.qml')}`;
}

// LG's compositor compiles a system UI's QML the first time it opens (~250 ms more): when the Settings
// key opens the Control Center, have it compiled once, unseen, a little after the service starts.
function preloadOverlay() {
  if (!fs.existsSync(OVERLAY_QML) || !(settingsKeyState().on || keyState(INPUT_KEY).on) || !process.getuid || process.getuid() !== 0) return;
  if (overlayOpen || overlayBusy) return; /* (it would close it) */
  luna(SYSUI + '/launch', {
    appId: OVERLAY_ID, main: overlayMain(), windowType: '_WEBOS_SYSTEM_UI_TYPE_OVERLAY', defaultWindowType: 'overlay',
    params: { preload: true }
  }).then((r) => log('control center overlay: preloaded', r && r.returnValue === false ? r.errorText : ''));
}
if (!process.env.HOMEPLUS_PREVIEW) setTimeout(preloadOverlay, 20000);

function closeOverlay() {
  overlayGen++; /* also one still on its way */
  if (!overlayOpen) return;
  overlayOpen = false;
  luna(SYSUI + '/closeByAppId', { appId: OVERLAY_ID });
}

// What it asks for (POST /api/overlay): {"event": "open", "target": "network" | "recents" |
// "accessibility" | "settings"}, or {"event": "open", "app"} for an input it offered; {"event":
// "opened"} (on screen) and {"event": "closed"}; and {"event": "failed", "error"}, when its panel
// didn't load: LG's Quick Settings instead.
async function overlayEvent(p) {
  if (p.event === 'frame') return nextFrame(Number(p.after) || 0); /* (answered when there's a newer one) */
  if (p.event === 'opened') {
    if (overlayAsked) log('control center overlay: on screen', Date.now() - overlayAsked, 'ms after the key');
    overlayAsked = 0;
  } else if (p.event === 'closed') {
    overlayOpen = false;
  } else if (p.event === 'failed') {
    overlayOpen = false;
    log('control center overlay failed:', String(p.error || '').replace(/\s+/g, ' ').slice(0, 300));
    await (overlayList === 'inputs' ? inputPicker() : quickSettings());
  } else if (p.event === 'open' && p.app) {
    if (!overlayInputs.includes(p.app)) throw new Error(`not an input: ${p.app}`);
    clearWake();
    await luna(APPMGR + '/launch', { id: p.app });
  } else if (p.event === 'open' && p.target === 'network') { /* as LG's Quick Settings open it */
    await luna(APPMGR + '/launch', { id: 'com.webos.app.firstuse-overlay', params: { target: 'network' } });
  } else if (p.event === 'open' && ['recents', 'accessibility', 'settings'].includes(p.target)) {
    emit(p.target); /* the launcher opens it once it's in front */
    await luna(APPMGR + '/launch', { id: NATIVE_ID });
  } else {
    throw new Error('not something the Control Center does');
  }
}

// ---- Events for the launcher: webOS gives a native app no way to hear from a service, so the
// launcher keeps a request open (GET /api/events), answered when something happens or after 15 s.

const listeners = new Set();
let unheard = []; /* events while the launcher had no request open (it sends the next at once) */

function emit(name) {
  if (!listeners.size) return unheard.push({ name, at: Date.now() });
  for (const res of listeners) sendJSON(res, 200, { events: [name] });
  listeners.clear();
}

function listen(req, res) {
  const recent = unheard.filter((e) => Date.now() - e.at < 3000).map((e) => e.name);
  unheard = [];
  if (recent.length) return sendJSON(res, 200, { events: recent });
  listeners.add(res);
  const t = setTimeout(() => { if (listeners.delete(res)) sendJSON(res, 200, { events: [] }); }, 15000);
  const done = () => { clearTimeout(t); listeners.delete(res); };
  res.on('finish', done); /* answered (Node 8 says only that: 'close' is for a hang-up there) */
  res.on('close', done);
}

function dropElevation() {
  // Homebrew Channel's elevation leaves per-service files under /var/luna-service2-dev
  const root = '/var/luna-service2-dev';
  let n = 0;
  try {
    for (const dir of fs.readdirSync(root)) {
      const d = path.join(root, dir);
      if (!fs.statSync(d).isDirectory()) continue;
      for (const f of fs.readdirSync(d)) {
        if (f.startsWith(NATIVE_ID) || f.startsWith(OLD_ID)) { remove.rmSync(path.join(d, f), { force: true }); n++; }
      }
    }
  } catch (e) { /* not elevated this way */ }
  return n;
}

async function uninstall() {
  const steps = {};
  await unmountRedirect().catch(() => {});
  steps.keyFilter = await keyFilter.remove().catch((e) => e.message); /* (out of the compositor's list; a reboot would do it too) */
  try { [INITD, BOOT_LOG].forEach((f) => remove.rmSync(f, { force: true })); steps.bootScript = true; } catch (e) { steps.bootScript = e.message; }
  try { /* (binds left alone keep their scripts: they may run one) */
    steps.inputhookBinds = dropInputhookBinds();
    Object.keys(KEY_SCRIPTS).forEach((f) => remove.rmSync(f, { force: true }));
  } catch (e) { steps.inputhookBinds = e.message; }
  try { remove.rmSync(path.dirname(findConfigPath()), { recursive: true, force: true }); steps.settings = true; } catch (e) { steps.settings = e.message; }
  steps.elevationFiles = dropElevation();
  log('uninstall:', steps);
  // removing the package stops this service, so it goes last, after the reply is sent
  setTimeout(async () => {
    await run('ls-control', ['scan-services']);
    const r = await luna('luna://com.webos.appInstallService/dev/remove', { id: NATIVE_ID });
    log('uninstall: remove package', r && r.returnValue === false ? r.errorText : 'requested');
  }, 800);
  return steps;
}

// When each app you opened was last in front. Recents lists only these, most recent first,
// like tvOS: webOS also runs apps nobody opened (preloaded for a quick start, or started
// again in the background after you closed them), and those stay out. Kept in /tmp, so a
// restarted service (an update) still knows them, and a reboot, which closes them, doesn't.
const RECENTS_FILE = process.env.HOMEPLUS_RECENTS_FILE || '/tmp/homeplus-recents.json';
const lastUsed = new Map((() => {
  try { return Object.entries(JSON.parse(tmp.read(RECENTS_FILE))); } catch (e) { return []; }
})());

function saveRecents() {
  const o = {};
  for (const [id, at] of lastUsed) o[id] = at;
  try { tmp.write(RECENTS_FILE, JSON.stringify(o)); } catch (e) { /* only for restarts */ }
}

async function onForeground(appId) {
  maybeHomeAfterWake(appId);
  if (!appId || appId === lastForeground) return;
  log('foreground', lastForeground || '-', '->', appId);
  lastForeground = appId;
  if (resumeRun && resumeRun.app === appId) resumeRun.seen = true;
  closeOverlay();
  lastUsed.set(appId, Date.now());
  saveRecents();
  const cfg = readConfig();
  const target = homeTarget(cfg);
  if (appId === target) takeoverLanded = true;
  if (cfg.homeOverride === false) return;
  if (!homeApps(cfg).includes(appId)) return;
  if (redirectTarget) return; // the no-flash page already opens Home+
  await takeover(target, '');
}

function homeApps(cfg) {
  return Array.isArray(cfg.homeApps) && cfg.homeApps.length ? cfg.homeApps : DEFAULT_HOME_APPS;
}

async function takeover(target, why) {
  // If the launcher never came up after a takeover moments ago, don't keep retrying.
  if (!takeoverLanded && Date.now() - lastTakeover < 3000) return;
  lastTakeover = Date.now();
  takeoverLanded = false;
  const r = await luna(APPMGR + '/launch', { id: target });
  log('home takeover' + why + ' ->', target, r && r.returnValue === false ? r.errorText : 'ok');
}

// ---- Back to Home+ when an app quits. With no app in front, SAM's last-app handler brings up its idle app a
// second later (settingsservice's lastAppHandlerPolicy "idleApp": configd's profile.idleApp, LG's home), and only
// then did the watch above replace it: LG's home showed for a moment at every exit. Home+, waiting in the
// background, is launched as soon as the app in front has quit and nothing's there, and SAM finds it in front when
// its second is up. Not for an app sent to the background (LG's compositor brings back the app before it), nor
// when another app is on its way to the front (50 ms are left for one to say so), nor for Home+ itself (closed,
// or being removed), nor while the TV isn't on.

const QUIT_GRACE_MS = 50;
const coming = new Map(); /* apps SAM is starting or bringing back to the front, id -> when */
const quit = new Map(); /* apps that closed, id -> when */
let emptied = { app: '', at: 0 }; /* the app that left the front with nothing after it, and when */
let tvOn = true;
let powerState = '';
let wakeUntil = 0;
let homeBeforeStandby = false;
let wakeTimer = null;

function clearWake() {
  wakeUntil = 0;
  if (wakeTimer) clearTimeout(wakeTimer);
  wakeTimer = null;
}

function maybeHomeAfterWake(appId) {
  if (!wakeUntil || !tvOn || !appId) return;
  if (Date.now() > wakeUntil) return clearWake();
  const cfg = readConfig();
  if (appId === homeTarget(cfg) || appId !== 'com.webos.app.livetv' || homeMode(cfg) === 'off') return clearWake();
  if (wakeTimer) return;
  wakeTimer = setTimeout(() => {
    wakeTimer = null;
    if (!wakeUntil || !tvOn || lastForeground !== 'com.webos.app.livetv') return;
    clearWake();
    takeover(homeTarget(readConfig()), ' (power on)').catch((e) => log('home wake failed', e.message));
  }, 650);
}

function onNoForeground() {
  const left = lastForeground;
  if (!left) return;
  log('foreground', left, '-> (none)');
  lastForeground = '';
  emptied = { app: left, at: Date.now() };
  setTimeout(homeAfterQuit, QUIT_GRACE_MS);
}

/* (SAM says an app closed and the screen has no app, in either order) */
function homeAfterQuit() {
  const left = emptied.app, now = Date.now();
  if (!left || lastForeground || now - emptied.at > 900 || !(now - (quit.get(left) || 0) < 3000)) return;
  emptied.app = '';
  const cfg = readConfig();
  const target = homeTarget(cfg);
  if (left === target || homeApps(cfg).includes(left) || cfg.homeOverride === false || redirectTarget || !tvOn) return;
  for (const [id, at] of coming) if (id !== target && now - at < 10000) return; /* (it'll be in front) */
  takeover(target, ` (${left} quit)`).catch((e) => log('home takeover failed', e.message));
}

const stremioServer = require('./stremio-server')({ luna, log });

function watchLife() {
  try {
    const sub = service.subscribe(APPMGR + '/getAppLifeEvents', { subscribe: true });
    stremioServer.check(); /* (each time it's subscribed: a Stremio that closed while it wasn't is caught up) */
    sub.on('response', (m) => {
      const p = m.payload || {};
      if (!p.appId) return;
      stremioServer.onLife(p.appId, p.event);
      if (p.event === 'close' || p.event === 'stop') {
        quit.set(p.appId, Date.now());
        if (p.appId === emptied.app) setTimeout(homeAfterQuit, QUIT_GRACE_MS);
      }
      if (p.event !== 'launch' && p.event !== 'splash') return coming.delete(p.appId);
      coming.set(p.appId, Date.now());
      // Home+ on its way back (the Home button, a takeover): the launcher, which only checks for that every
      // 100 ms while hidden, hears it now
      if (p.appId === NATIVE_ID && lastForeground !== NATIVE_ID) emit('wake');
    });
    sub.on('cancel', () => { log('app life subscription cancelled; retrying'); setTimeout(watchLife, 3000); });
  } catch (e) {
    log('app life watch failed', e.message);
    setTimeout(watchLife, 10000);
  }
}
watchLife();

function watchPower() {
  try {
    const sub = service.subscribe('luna://com.webos.service.tvpower/power/getPowerState', { subscribe: true });
    sub.on('response', (m) => {
      const p = m.payload || {};
      if (!p.state) return;
      tvOn = p.state === 'Active' && !p.processing; /* (not turning off, nor its screen) */
      const previous = powerState;
      if (p.state !== 'Active' || !p.processing) powerState = p.state;
      if (['Active Standby', 'Suspend', 'Power Off'].includes(p.state) &&
          !['Active Standby', 'Suspend', 'Power Off'].includes(previous)) {
        const target = homeTarget(readConfig());
        // webOS may clear the foreground just before reporting standby.
        homeBeforeStandby = lastForeground === target ||
          (!lastForeground && emptied.app === target && Date.now() - emptied.at < 2000);
        clearWake();
      }
      if (!tvOn || !['Active Standby', 'Suspend', 'Power Off'].includes(previous)) return;
      log('power wake', previous, '-> Active');
      if (!homeBeforeStandby) return;
      homeBeforeStandby = false;
      wakeUntil = Date.now() + 5000;
      const until = wakeUntil;
      setTimeout(() => { if (wakeUntil === until && lastForeground === 'com.webos.app.livetv') maybeHomeAfterWake(lastForeground); }, 100);
      setTimeout(() => { if (wakeUntil === until) clearWake(); }, 5000);
    });
    sub.on('cancel', () => setTimeout(watchPower, 3000));
  } catch (e) {
    log('power watch failed', e.message);
    setTimeout(watchPower, 10000);
  }
}
watchPower();

function watchForeground() {
  try {
    const sub = service.subscribe(APPMGR + '/getForegroundAppInfo', { subscribe: true });
    sub.on('response', (m) => {
      const p = m.payload || {};
      const info = Array.isArray(p.foregroundAppInfo) ? p.foregroundAppInfo : null;
      if (p.appId === '' || (info && !info.length)) return onNoForeground(); /* (no app in front: one may have quit) */
      onForeground(p.appId || (info && info[0] && info[0].appId));
    });
    sub.on('cancel', () => { log('foreground subscription cancelled; retrying'); setTimeout(watchForeground, 3000); });
  } catch (e) {
    log('foreground watch failed', e.message);
    setTimeout(watchForeground, 10000);
  }
}
watchForeground();

register('setHomeOverride', (msg) => {
  const cfg = readConfig();
  const p = msg.payload || {};
  if (typeof p.enabled === 'boolean') cfg.homeOverride = p.enabled;
  if (Array.isArray(p.homeApps)) cfg.homeApps = p.homeApps.map(String);
  if (typeof p.target === 'string') cfg.homeTarget = p.target || undefined;
  if (['redirect', 'watch'].includes(p.mode)) cfg.homeMode = p.mode;
  writeConfig(cfg);
  // rebuild: the no-flash page has the target baked in
  unmountRedirect().then(applyHomeMode).catch((e) => log('home mode error', e.message));
  msg.respond({ returnValue: true, enabled: cfg.homeOverride !== false, mode: homeMode(cfg),
    homeApps: cfg.homeApps || DEFAULT_HOME_APPS, target: cfg.homeTarget || 'auto', lastForeground });
});

// ------------------------------------------------------------------ local JSON API
// Used by the native launcher, which can't speak Luna
// easily. Same rules as the image server: 127.0.0.1 only.

const HIDDEN_APPS = [NATIVE_ID, OLD_ID, 'com.webos.app.home',
  'com.webos.app.homeconnect', 'com.webos.app.voice'];

function hexColor(c) {
  const m = /^#?([0-9a-f]{6})$/i.exec(c || '');
  return m ? '#' + m[1].toLowerCase() : '';
}

// Stremio artwork is fetched through the service so the native app needn't do TLS.
// Only URLs that came back in our own Stremio results can be proxied.
const remoteImages = new Map(); // key -> url
function proxied(url) {
  if (!url) return '';
  const key = require('crypto').createHash('sha1').update(url).digest('hex').slice(0, 16);
  remoteImages.set(key, url);
  return `/img/${key}`;
}

// The pictures are kept on disk, so a title's picture is there at once the next time it's
// on show (the TV takes ~250 ms to download one). Up to 64 MB: the least recently shown go.
const IMAGE_CACHE_MAX = 64 * 1024 * 1024;
let imageDir = '', parts = 0, trimTimer = null;

// They come from Stremio's addons, your Jellyfin and Plex servers and TMDB, and the service runs as
// root, so it takes only so much: a picture up to 8 MB (TMDB's largest are about 3), from a server
// that keeps sending (15 s without a byte and it's given up) and is done within 30 s, its redirects
// included (one trickling in a byte at a time is given up then), redirected only to another web server
const IMAGE_MAX_BYTES = 8 * 1024 * 1024;
const IMAGE_TIMEOUT_MS = Number(process.env.HOMEPLUS_FETCH_TIMEOUT_MS) || 15000; /* tests: shorter */
const IMAGE_DEADLINE_MS = Number(process.env.HOMEPLUS_FETCH_DEADLINE_MS) || 30000; /* (the launcher's for it is 40 s) */

function imageCacheDir() {
  if (!imageDir) {
    imageDir = path.join(path.dirname(findConfigPath()), 'images');
    mkdirSync(imageDir, 0o700); /* what you watch: root's only */
  }
  return imageDir;
}

function sendImage(key, res) {
  let file = '';
  try { file = path.join(imageCacheDir(), key); } catch (e) { /* no cache: straight from the internet */ }
  if (!file) return fetchRemote(remoteImages.get(key), res);
  fs.readFile(file, (err, buf) => {
    if (err || !buf.length) return fetchRemote(remoteImages.get(key), res, 0, file);
    const now = new Date();
    fs.utimes(file, now, now, () => {}); /* recently shown */
    res.writeHead(200, { 'Content-Type': buf[0] === 0x89 ? 'image/png' : 'image/jpeg', 'Content-Length': buf.length, 'Cache-Control': 'max-age=86400' });
    res.end(buf);
  });
}

// a downloaded picture goes into the cache once it has arrived whole; cut short (the server went,
// stopped sending, or sent too much), what came of it goes
function keepImage(up, file) {
  const part = `${file}.${++parts}.part`;
  const out = fs.createWriteStream(part, { mode: 0o600 });
  let ended = false, dropped = false;
  const drop = () => {
    if (dropped) return;
    dropped = true;
    out.destroy();
    out.once('close', () => fs.unlink(part, () => {})); /* (once closed: it may still have been opening) */
  };
  up.on('end', () => { ended = true; });
  up.on('aborted', drop);
  up.on('error', drop);
  up.on('close', () => { if (!ended) drop(); });
  out.on('error', drop);
  out.on('finish', () => {
    if (!ended || up.complete === false) return fs.unlink(part, () => {});
    fs.rename(part, file, () => trimImages());
  });
  up.pipe(out);
}

function trimImages() {
  if (trimTimer) return;
  trimTimer = setTimeout(() => {
    trimTimer = null;
    const files = [];
    let total = 0;
    try {
      for (const f of fs.readdirSync(imageDir)) {
        try {
          const st = fs.statSync(path.join(imageDir, f));
          files.push({ f, at: st.mtimeMs, size: st.size });
          total += st.size;
        } catch (e) { /* just went */ }
      }
      if (total <= IMAGE_CACHE_MAX) return;
      files.sort((a, b) => a.at - b.at);
      for (const x of files) {
        if (total <= IMAGE_CACHE_MAX * 0.8) break;
        remove.rmSync(path.join(imageDir, x.f), { force: true });
        total -= x.size;
      }
    } catch (e) {
      log('image cache:', e.message);
    }
  }, 10000);
}

// where a picture may come from: another web server, not this TV (its own services answer on
// 127.0.0.1); servers on the network stay allowed, as Jellyfin's and Plex's are. Checked for the
// first URL as for every redirect, as the URLs come from addons and servers. null: nowhere
const onTV = onThisTV;
/* (the tests' pictures, on a server of theirs on 127.0.0.1; HOMEPLUS_PREVIEW_ART_STRICT: not even those) */
const PREVIEW_ART = !process.env.HOMEPLUS_PREVIEW_ART_STRICT && process.env.HOMEPLUS_PREVIEW_ART;
function pictureURL(location, from, first) {
  let u;
  try { u = new URL(location, from); } catch (e) { return null; }
  const host = u.hostname.replace(/^\[|\]$/g, '').replace(/\.$/, '').toLowerCase(); /* (the URL has made 127.1 and the like 127.0.0.1) */
  const ours = first && PREVIEW_ART && u.href.startsWith(PREVIEW_ART + '/');
  return (u.protocol === 'http:' || u.protocol === 'https:') && (ours || !onTV(host)) ? u.toString() : null;
}

// and a name that turns out to be the TV (127.0.0.1.nip.io and the like) is refused when it's looked up
function lookupOffTV(hostname, opts, cb) {
  dns.lookup(hostname, opts, (err, addr, family) => {
    if (err) return cb(err);
    const all = Array.isArray(addr) ? addr : [{ address: addr, family }];
    if (all.some((a) => onTV(String(a.address).toLowerCase()))) return cb(Object.assign(new Error(`${hostname} is the TV itself`), { code: 'EACCES' }));
    cb(null, addr, family);
  });
}

function fetchRemote(url, res, depth = 0, file = '', started = Date.now()) {
  let u = depth === 0 ? pictureURL(url, undefined, true) : url;
  if (!u) log('image: a picture from the TV itself, or not from a web server, refused');
  try { u = u && new URL(u); } catch (e) { u = null; }
  const mod = u && u.protocol === 'https:' ? https : u && u.protocol === 'http:' ? http : null;
  if (!mod) { res.writeHead(404); return res.end(); }
  let answered = false; /* a response came: its end says how it went */
  const req = mod.get(requestOptions(u, { timeout: IMAGE_TIMEOUT_MS, lookup: lookupOffTV }), (up) => {
    answered = true;
    up.on('error', () => {}); /* (cut short: 'close' below sees to it) */
    if (up.statusCode >= 300 && up.statusCode < 400 && up.headers.location && depth < 4) {
      up.resume();
      const next = pictureURL(up.headers.location, url, false);
      if (next) return fetchRemote(next, res, depth + 1, file, started);
      log('image: a redirect to the TV itself, or not to a web server, refused');
      res.writeHead(502);
      return res.end();
    }
    if (up.statusCode !== 200) { up.resume(); res.writeHead(404); return res.end(); }
    if (Number(up.headers['content-length']) > IMAGE_MAX_BYTES) { /* too big to take */
      up.destroy();
      res.writeHead(502);
      return res.end();
    }
    res.writeHead(200, { 'Content-Type': up.headers['content-type'] || 'image/jpeg', 'Cache-Control': 'max-age=86400' });
    let got = 0, ended = false;
    up.on('data', (c) => { if ((got += c.length) > IMAGE_MAX_BYTES) up.destroy(); }); /* too big after all: not kept */
    // Node 12 can emit 'end' after 'aborted', before 'close': stop the pipe before that end
    // can finish a partial image as a whole response (or leave its socket open for another one).
    up.on('aborted', () => res.destroy());
    up.on('end', () => { ended = true; });
    up.on('close', () => { if (!ended || up.complete === false) res.destroy(); }); /* cut short: so is the launcher's */
    // The launcher hung up (it dropped the picture: the Top Shelf moved on): it still comes into the cache, there
    // the next time it's on show; with no cache to keep it in, its download stops
    res.on('close', () => { if (!ended && !file) up.destroy(); });
    up.pipe(res);
    if (file) keepImage(up, file);
  });
  req.on('timeout', () => req.destroy(new Error('timed out'))); /* the server stopped sending */
  /* too long in all: cut short as when it stops sending (a redirect goes on with what's left) */
  const late = setTimeout(() => req.destroy(new Error('took too long')), Math.max(0, started + IMAGE_DEADLINE_MS - Date.now()));
  /* (Node 8 sends no 'close' after a whole response, only 'end') */
  req.on('response', (up) => { up.on('end', () => clearTimeout(late)); up.on('close', () => clearTimeout(late)); });
  req.on('error', () => {
    clearTimeout(late);
    if (answered) return;
    answered = true;
    res.writeHead(502);
    res.end();
  });
}

// ------------------------------------------------------------------ API access
// The service runs as root and listens on 127.0.0.1, where any other app, or a
// web page through DNS rebinding, could reach it. So only the launcher may use
// it: each request needs this per-start token, which is written into the
// launcher's own folder (other apps are jailed and can't read it), and requests
// from a browser (an Origin header) or for a non-loopback host are refused.
const crypto = require('crypto');
const TOKEN = crypto.randomBytes(24).toString('hex');

function writeToken() {
  const files = [path.join(__dirname, '..', '..', 'applications', NATIVE_ID, 'api-token'), process.env.HOMEPLUS_TOKEN_FILE];
  let written = 0;
  for (const f of files.filter(Boolean)) {
    /* written beside it, then renamed over it: a link put in its place is replaced, never followed (as root) */
    const part = `${f}.${process.pid}.part`;
    try {
      remove.rmSync(part, { force: true });
      const fd = fs.openSync(part, fs.constants.O_WRONLY | fs.constants.O_CREAT | fs.constants.O_EXCL | (fs.constants.O_NOFOLLOW || 0), 0o644);
      try { fs.writeSync(fd, TOKEN); fs.fchmodSync(fd, 0o644); } finally { fs.closeSync(fd); } /* (the launcher reads it: not root) */
      fs.renameSync(part, f);
      written++;
    } catch (e) { remove.rmSync(part, { force: true }); /* not installed there, or not root yet */ }
  }
  return written > 0;
}
let tokenWritten = writeToken();

function allowed(req) {
  if (!/^(127\.0\.0\.1|localhost)(:\d+)?$/.test(String(req.headers.host || ''))) return false;
  if (req.headers.origin) return false;
  const got = Buffer.from(String(req.headers['x-homeplus-token'] || '')), want = Buffer.from(TOKEN);
  return got.length === want.length && crypto.timingSafeEqual(got, want);
}

function sendJSON(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve) => {
    let buf = '';
    req.on('data', (c) => { if (buf.length < 65536) buf += c; });
    req.on('end', () => { try { resolve(JSON.parse(buf || '{}')); } catch (e) { resolve({}); } });
  });
}

// Apps you can delete from the TV (like tvOS's Delete App): those webOS says can be removed, but
// not what roots the TV (Homebrew Channel, and LG Input Hook) or Home+ itself (Remove Home+ first
// undoes what it set up).
const KEEP_APPS = ['org.webosbrew.hbchannel', 'org.webosbrew.inputhook', NATIVE_ID];
const deletable = (a) => a.removable === true && !a.systemApp && !KEEP_APPS.includes(a.id);

// Deletes an app as LG's home does: a store app by removing its launch point (webOS then uninstalls
// it), a developer one with appInstallService's dev/remove. It leaves Recents too.
async function deleteApp(id) {
  const a = (((await listApps()) || {}).apps || []).find((x) => x.id === id);
  if (!a || !deletable(a)) throw new Error(`${a ? a.title || id : id} can't be deleted`);
  const dev = String(a.folderPath || '').startsWith('/media/developer/');
  const r = dev ? await luna('luna://com.webos.appInstallService/dev/remove', { id })
    : await luna(APPMGR + '/removeLaunchPoint', { launchPointId: `${id}_default` });
  if (!r || r.returnValue === false) throw new Error((r && r.errorText) || 'the TV didn\'t delete it');
  log('deleted', id, dev ? '(developer app)' : '');
  lastUsed.delete(id);
  saveRecents();
  return { returnValue: true, deleted: id };
}

async function apiApps() {
  const r = await listApps();
  if (!r || !Array.isArray(r.apps)) throw new Error(`the TV didn't list its apps: ${(r && r.errorText) || 'no answer'}`); /* (not an empty list) */
  return r.apps
    .filter((a) => a.visible !== false && !HIDDEN_APPS.includes(a.id) && !a.id.startsWith('com.atvlauncher.')) // and the pre-1.0 Home+ packages
    .map((a) => {
      const e = images[a.id] || {};
      return {
        id: a.id,
        title: a.title || a.id,
        icon: e.icon && e.icon.length ? `/icon/${encodeURIComponent(a.id)}` : '',
        art: e.art && e.art.length ? `/art/${encodeURIComponent(a.id)}` : '',
        color: hexColor(a.iconColor) || hexColor(a.bgColor),
        removable: deletable(a)
      };
    });
}

const trakt = require('./trakt')({ readConfig, writeConfig, log });

// The TV's store country (LG's, which matches the apps it offers): TMDB's region.
let tvCountry = null;
async function tvRegion() {
  if (tvCountry === null) {
    const r = await luna('luna://com.webos.settingsservice/getSystemSettings', { category: 'option', keys: ['smartServiceCountryCode2'] });
    tvCountry = (r && r.settings && r.settings.smartServiceCountryCode2) || '';
  }
  return tvCountry;
}
const tmdb = require('./tmdb')({ readConfig, writeConfig, log, region: tvRegion });
// TMDB's and Trakt's keys, pasted on a phone: a page on the local network while the launcher shows its QR code
const keysPage = require('./keys')({ tmdb, trakt, log, emit });
const ambient = require('./ambient')({ luna, log });
const control = require('./control')({ luna, log, readConfig, writeConfig, extra: ambient }); /* the Control Center's TV controls, and ambient light (HyperHDR, PicCap) */
const picCapture = ambient.find((c) => c.id === 'screenCapture'); /* Blur Over Video pauses it */

// What's popular on the service a streaming app is, or trending for an app with no picture,
// for its Top Shelf (pictures through us).
async function apiFeatured(id) {
  if (!titles[id]) await listApps();
  const hasArt = !!(images[id] && images[id].art && images[id].art.length); /* else: what's trending */
  const f = await tmdb.featured(titles[id] ? { id, title: titles[id] } : null, hasArt);
  return { kind: f.kind, provider: f.provider, items: f.items.map((it) => Object.assign({}, it, { background: proxied(it.background), logo: proxied(it.logo) })) };
}
// Media servers: each one's Continue Watching shows in its own app's Top Shelf.
const media = {
  jellyfin: require('./jellyfin')({ readConfig, writeConfig, log, version: pkg.version }),
  plex: require('./plex')({ readConfig, writeConfig, log, version: pkg.version })
};

// Continue Watching from every connected source: Stremio's library and Trakt's
// paused playback, newest first. Stremio scrobbles to Trakt too, so the same
// title can come from both; keep whichever was watched most recently.
// One media server's row (artwork through the service: its URLs carry tokens).
async function apiMediaContinue(source) {
  const m = media[source];
  let items = [];
  try { items = await m.items(); } catch (e) { log(source, 'continue failed', e.message); }
  items = items.map((it) => Object.assign({}, it, { background: proxied(it.background), thumbnail: proxied(it.thumbnail), logo: proxied(it.logo) }));
  return { source, connected: m.status().connected, items };
}

async function apiContinue() {
  const cfg = readConfig();
  const sources = [];
  if (cfg.stremioAuthKey) {
    sources.push((async () => {
      if (!cache.items || Date.now() - cache.at > CACHE_MS) {
        try {
          cache = { at: Date.now(), items: await continueWatching(cfg.stremioAuthKey) };
        } catch (e) { /* an outage doesn't empty the shelf: the last list stays, and we try again in a minute */
          if (!cache.items) throw e;
          log('stremio: refresh failed, keeping the last list:', e.message);
          cache.at = Date.now() - CACHE_MS + 60 * 1000;
        }
      }
      return cache.items.map((it) => Object.assign({ source: 'stremio' }, it));
    })());
  }
  sources.push(trakt.playback());
  const lists = await Promise.all(sources.map((p) => p.catch((e) => { log('continue source failed', e.message); return []; })));
  const byId = new Map();
  for (const it of [].concat(...lists)) {
    const prev = byId.get(it.id);
    if (!prev || String(it.lastWatched) > String(prev.lastWatched)) byId.set(it.id, it);
  }
  const selected = sortStable([...byId.values()], (a, b) => String(b.lastWatched).localeCompare(String(a.lastWatched))).slice(0, 16);
  const items = await Promise.all(selected.map(async (it) => {
    const details = cfg.prefs && cfg.prefs.topShelfDetails === 'off' ? null : await tmdb.detailsByImdb(it.id, it.type);
    return Object.assign({}, it, details || {}, {
      background: proxied(it.background), thumbnail: proxied(it.thumbnail), logo: proxied(it.logo), poster: proxied(it.poster)
    });
  }));
  const t = trakt.status();
  return {
    source: 'stremio', connected: !!cfg.stremioAuthKey || t.connected, stremio: !!cfg.stremioAuthKey, trakt: t, items,
    jellyfin: media.jellyfin.status(), plex: media.plex.status(), tmdb: tmdb.status() // for Settings -> Accounts
  };
}

// ---- settings apps for the native Control Center (no inputs: the remote's Input key has LG's own menu)

const SETTINGS_APPS = [
  ['com.palm.app.settings', 'All Settings'],
  ['com.webos.app.quicksettings', 'Quick Settings'],
  ['com.webos.app.picturesettings', 'Picture'],
  ['com.webos.app.soundsettings', 'Sound'],
  ['com.webos.app.inputmgr', 'Input Manager']
];

async function apiSystem() {
  const all = ((await listApps()) || {}).apps || [];
  const ids = new Set(all.map((a) => a.id));
  const settings = SETTINGS_APPS.filter(([id]) => ids.has(id)).map(([id, label]) => ({ id, label }));
  return { settings };
}

// Recent apps: the apps you opened that are still running (only apps on the home screen,
// not Home+ itself), most recently used first. One that stopped is forgotten, so webOS
// starting it again in the background doesn't bring it back.
async function apiRunning() {
  const [r, apps] = await Promise.all([luna(APPMGR + '/running', {}), apiApps()]);
  const byId = new Map(apps.map((a) => [a.id, a]));
  const running = new Set(((r && r.running) || []).map((x) => x.id));
  if (r && r.returnValue !== false) {
    for (const id of [...lastUsed.keys()]) if (!running.has(id)) lastUsed.delete(id);
    saveRecents();
  }
  const opened = [...running].filter((id) => byId.has(id) && lastUsed.has(id));
  opened.sort((a, b) => lastUsed.get(b) - lastUsed.get(a));
  return { apps: opened.map((id) => byId.get(id)) };
}

const showToast = (message) => luna('luna://com.webos.notification/createToast', { sourceId: NATIVE_ID, message: String(message).slice(0, 200) });
let hint = null; /* the toast waiting for its app to open: { message, timer } */

// Opens an app. toast: a hint shown by the system, so it appears over the app that just opened.
async function openApp(id, params, toast) {
  clearWake();
  const r = await luna(APPMGR + '/launch', params ? { id, params } : { id });
  if (toast && r && r.returnValue !== false) {
    if (hint) clearTimeout(hint.timer);
    const h = hint = { message: toast, timer: setTimeout(() => { hint = null; showToast(h.message); }, 1500) };
  }
  return r;
}

// What happened since: the hint says this instead, or, gone already, this follows it.
function tellInstead(message) {
  if (hint) hint.message = message;
  else showToast(message);
}

// OK on a title in the Top Shelf: its app has opened, and now its server has it play the title. The launcher has
// its answer by then: this takes seconds, and is all that polls, only after an OK. A new run replaces the last, and
// once the app has been in front any other app there ends it (right after the launch Home+ still is, which doesn't
// count), so nothing starts behind your back. If it can't be done, the toast OK has always shown.
async function playInBackground(m, app, item, fromStart, run) {
  const stillWanted = () => run === resumeRun && !(run.seen && lastForeground && lastForeground !== app);
  if (await m.playOnTV({ id: item.id, fromStart }, stillWanted) || !stillWanted()) return;
  if (!titles[app]) await listApps();
  tellInstead(`“${item.name}” is in ${titles[app] || app}’s Continue Watching`);
}

async function handleApi(req, res) {
  const url = new URL(req.url, 'http://127.0.0.1');
  try {
    if (url.pathname === '/api/apps') return sendJSON(res, 200, { apps: await apiApps() });
    if (url.pathname === '/api/stremio' || url.pathname === '/api/continue') {
      const source = url.searchParams.get('source');
      return sendJSON(res, 200, media[source] ? await apiMediaContinue(source) : await apiContinue());
    }
    if (url.pathname === '/api/featured') return sendJSON(res, 200, await apiFeatured(String(url.searchParams.get('app') || '')));
    if (url.pathname === '/api/overlay' && req.method === 'POST') { /* the Control Center over other apps */
      return sendJSON(res, 200, (await overlayEvent(await readBody(req))) || { returnValue: true });
    }
    if (url.pathname === '/api/control') { /* the Control Center: GET its state, POST {action, value} */
      if (req.method === 'POST') {
        const b = await readBody(req);
        if (b.action === 'input') clearWake();
        await control.act(String(b.action || ''), b.value, b);
        log('control:', b.action, b.value === undefined ? '' : b.value);
      }
      return sendJSON(res, 200, await control.state());
    }
    const mm = /^\/api\/(stremio|jellyfin|plex)\/(status|link|disconnect|server)$/.exec(url.pathname);
    if (mm) { /* sign-in: a code (and a QR code, through us, for Stremio and Plex) approved elsewhere */
      const m = mm[1] === 'stremio' ? { status: stremioStatus, startLink: stremioStartLink, disconnect: stremioSignOut } : media[mm[1]];
      const withQr = (st) => (st.pending && st.pending.qr ? Object.assign({}, st, { pending: Object.assign({}, st.pending, { qr: proxied(st.pending.qr) }) }) : st);
      if (mm[2] === 'status') return sendJSON(res, 200, withQr(m.status()));
      if (req.method !== 'POST') return sendJSON(res, 405, { returnValue: false, errorText: 'POST only' });
      if (mm[2] === 'link') return sendJSON(res, 200, withQr(await m.startLink()));
      if (mm[2] === 'disconnect') return sendJSON(res, 200, await m.disconnect());
      if (mm[2] === 'server' && m.setServer) return sendJSON(res, 200, m.setServer((await readBody(req)).url));
    }
    if (url.pathname === '/api/system') return sendJSON(res, 200, await apiSystem());
    if (url.pathname === '/api/running') return sendJSON(res, 200, await apiRunning());
    if (url.pathname === '/api/close' && req.method === 'POST') {
      const b = await readBody(req);
      const ids = b.all ? (await apiRunning()).apps.map((a) => a.id) : [String(b.id || '')].filter(Boolean);
      for (const id of ids) {
        await luna(APPMGR + '/closeByAppId', { id });
        lastUsed.delete(id); /* out of Recents until you open it again */
      }
      saveRecents();
      log('closed', ids);
      return sendJSON(res, 200, { returnValue: true, closed: ids });
    }
    if (url.pathname === '/api/remove' && req.method === 'POST') return sendJSON(res, 200, await deleteApp(String((await readBody(req)).id || '')));
    if (url.pathname === '/api/uninstall' && req.method === 'POST') {
      return sendJSON(res, 200, Object.assign({ returnValue: true }, await uninstall()));
    }
    if (url.pathname === '/api/home') { /* (the config is read once the body is in: a sign-in may save it meanwhile) */
      const b = req.method === 'POST' ? await readBody(req) : null;
      const cfg = readConfig();
      if (b) {
        if (typeof b.settingsKey === 'boolean' || typeof b.inputKey === 'boolean') {
          const was = { settingsKey: cfg.settingsKey, inputKey: cfg.inputKey };
          if (typeof b.settingsKey === 'boolean') setKeyBind(SETTINGS_KEY, b.settingsKey, cfg); /* the Settings key's bind */
          if (typeof b.inputKey === 'boolean') setKeyBind(INPUT_KEY, b.inputKey, cfg); /* the Input key's */
          const took = await applyKeyFilter();
          if (took !== true && !fs.existsSync(path.dirname(INPUTHOOK_BINDS))) { /* the key filter is all that catches them: as they were */
            Object.assign(cfg, was);
            writeConfig(cfg);
            await applyKeyFilter();
            throw new Error(`the TV didn't take Home+'s key filter: ${took}`); /* (Settings says so) */
          }
        }
        if (typeof b.onboarded === 'boolean') { /* the welcome steps: false while they're on, true once done or skipped */
          cfg.onboarded = b.onboarded;
          if (b.mode === undefined && !b.asked) writeConfig(cfg);
        }
        if (b.mode !== undefined || b.asked) {
          if (['redirect', 'watch'].includes(b.mode)) { cfg.homeMode = b.mode; cfg.homeOverride = true; }
          if (b.mode === 'off') cfg.homeOverride = false;
          if (b.asked) cfg.homeAsked = true;
          writeConfig(cfg);
          await unmountRedirect();
          await applyHomeMode();
        }
      }
      return sendJSON(res, 200, { mode: homeMode(cfg), noFlashActive: !!redirectTarget || keyFilter.takes('home'), noFlashAvailable: await noFlashAvailable(), settingsKey: settingsKeyState(), inputKey: keyState(INPUT_KEY),
        piccap: !!picCapture && await picCapture.available() }); /* Settings -> Control Center offers Blur Over Video */
    }
    if (url.pathname === '/api/events') return listen(req, res);
    if (url.pathname === '/api/key' && req.method === 'POST') return sendJSON(res, 200, await remoteKey(String((await readBody(req)).key || '')));
    if (url.pathname === '/api/keys') return sendJSON(res, 200, keysPage.status());
    if (url.pathname === '/api/keys/open' && req.method === 'POST') {
      try { return sendJSON(res, 200, await keysPage.open()); } catch (e) { return sendJSON(res, 200, { open: false, errorText: e.message }); }
    }
    if (url.pathname === '/api/keys/close' && req.method === 'POST') { keysPage.close(); return sendJSON(res, 200, keysPage.status()); }
    if (url.pathname === '/api/keys/qr') {
      const png = keysPage.qrPng();
      if (!png) return sendJSON(res, 404, { returnValue: false, errorText: 'the keys page isn\'t open' });
      res.writeHead(200, { 'Content-Type': 'image/png', 'Content-Length': png.length, 'Cache-Control': 'no-store' });
      return res.end(png);
    }
    if (url.pathname === '/api/trakt/status') return sendJSON(res, 200, trakt.status());
    if (url.pathname === '/api/trakt/link' && req.method === 'POST') return sendJSON(res, 200, await trakt.startDevice());
    if (url.pathname === '/api/trakt/disconnect' && req.method === 'POST') return sendJSON(res, 200, trakt.disconnect());
    if (url.pathname === '/api/launch' && req.method === 'POST') {
      const { id, params, toast } = await readBody(req);
      if (!id) return sendJSON(res, 400, { returnValue: false, errorText: 'id is required' });
      return sendJSON(res, 200, await openApp(id, params, toast));
    }
    if (url.pathname === '/api/resume' && req.method === 'POST') { /* OK on a Jellyfin card: { app, source, id, fromStart } */
      const { app, source, id, fromStart } = await readBody(req);
      const m = media[source], run = resumeRun = { app, seen: false };
      if (!app || !m || !m.playOnTV) return sendJSON(res, 400, { returnValue: false, errorText: 'app and a source that can play are required' });
      const item = (await m.items()).find((it) => it.id === id);
      if (!item) return sendJSON(res, 404, { returnValue: false, errorText: 'that title isn\'t in the list' });
      const r = await openApp(app, null, `${item.positionTicks && !fromStart ? 'Resuming' : 'Playing'} “${item.name}”`);
      if (r && r.returnValue !== false) playInBackground(m, app, item, !!fromStart, run).catch((e) => log('resume failed', e.message));
      return sendJSON(res, 200, r);
    }
    if (url.pathname === '/api/layout') {
      const l = req.method === 'POST' ? await readBody(req) : null;
      const cfg = readConfig();
      if (l) {
        cfg.layout = { order: Array.isArray(l.order) ? l.order.map(String) : [], hidden: Array.isArray(l.hidden) ? l.hidden.map(String) : [] };
        writeConfig(cfg);
      }
      return sendJSON(res, 200, cfg.layout || { order: [], hidden: [] });
    }
    if (url.pathname === '/api/prefs') { // launcher preferences, e.g. {"sounds": false}
      const b = req.method === 'POST' ? await readBody(req) : null;
      const cfg = readConfig();
      if (b) {
        cfg.prefs = Object.assign({}, cfg.prefs, b);
        writeConfig(cfg);
      }
      return sendJSON(res, 200, cfg.prefs || {});
    }
    if (url.pathname === '/api/log' && req.method === 'POST') {
      const b = await readBody(req);
      log('native:', String(b.message || '').slice(0, 500));
      return sendJSON(res, 200, { returnValue: true });
    }
    const img = /^\/img\/([0-9a-f]{16})$/.exec(url.pathname);
    if (img && remoteImages.has(img[1])) return sendImage(img[1], res);
    sendJSON(res, 404, { returnValue: false, errorText: 'not found' });
  } catch (e) {
    log('api error', url.pathname, e.message);
    sendJSON(res, 500, { returnValue: false, errorText: e.message });
  }
}

const imageServer = http.createServer(async (req, res) => {
  /* without the token: only whether Home+ is set up (the launcher asks before it has one) */
  if ((req.url || '') === '/api/status' && !req.headers.origin) {
    tokenWritten = writeToken(); /* again: installing the launcher replaces its folder */
    const cfg = readConfig();
    return sendJSON(res, 200, {
      id: pkg.name, /* (the launcher checks it's its own: the old Home+'s, still installed, takes the same port) */
      version: pkg.version, elevated: isRoot(), token: tokenWritten,
      bootScript: fs.existsSync(INITD), homeAsked: !!cfg.homeAsked, homeMode: homeMode(cfg), onboarded: onboarded(cfg),
      tmdbConfigured: !!cfg.tmdbKey
    });
  }
  /* the launcher just had Homebrew Channel elevate us: exit so the next start runs as root.
   * Refused once elevated, so it can't be used to knock a working service over. */
  if ((req.url || '') === '/api/restart' && req.method === 'POST' && !req.headers.origin && !isRoot()) {
    log('restart requested (not elevated yet)');
    sendJSON(res, 200, { returnValue: true });
    setTimeout(() => process.exit(0), 200);
    return;
  }
  if (!allowed(req)) {
    res.writeHead(403);
    res.end();
    return;
  }
  if (/^\/(api|img)\//.test(req.url || '')) return handleApi(req, res);
  const m = /^\/(icon|art)\/([^/?]+)/.exec(req.url || '');
  let id = null;
  try { id = m ? decodeURIComponent(m[2]) : null; } catch (e) { /* malformed: not found */ }
  const file = id ? await imageFile(m[1], id) : null;
  if (!file) {
    res.writeHead(404);
    res.end();
    return;
  }
  res.writeHead(200, {
    'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
    'Cache-Control': 'max-age=604800'
  });
  fs.createReadStream(file).on('error', () => res.end()).pipe(res);
});
imageServer.headersTimeout = 10000; /* a request never sent whole doesn't keep its socket (Node 16 sets no limit; */
imageServer.requestTimeout = 30000; /* Node 8 has neither, but closes a socket idle for 2 minutes) */
imageServer.on('error', (e) => log('image server error', e.message));
imageServer.listen(IMAGE_PORT, '127.0.0.1', () => log('image server listening on', IMAGE_PORT));

// ---- Trakt over Luna (for setup from a terminal)
// The Control Center's actions, for remote buttons through LG Input Hook: {"action": "powerOff" |
// "screenOff" | "sound" | "sleepTimer" | "input", "value": ...} (see service/control.js).
register('control', async (msg) => {
  const p = msg.payload || {};
  try {
    await control.act(String(p.action || ''), p.value, p);
    msg.respond(Object.assign({ returnValue: true }, await control.state()));
  } catch (e) {
    fail(msg, e.message);
  }
});

// TMDB, for what's popular on streaming apps: {"key"} is a v3 API key or a Read Access Token
// ("" removes it); {"region"} the country whose catalogs to show ("" is the TV's store country).
register('tmdbSetup', async (msg) => {
  const p = msg.payload || {};
  try {
    let st = tmdb.status();
    if ('region' in p) st = tmdb.setRegion(p.region);
    if ('key' in p) st = await tmdb.setKey(p.key);
    msg.respond(Object.assign({ returnValue: true }, st));
  } catch (e) {
    fail(msg, e.message);
  }
});

register('traktSetup', (msg) => {
  const p = msg.payload || {};
  if (!p.clientId) return fail(msg, 'clientId is required');
  msg.respond(Object.assign({ returnValue: true }, trakt.setup(p.clientId, p.clientSecret || '')));
});
register('traktStatus', (msg) => msg.respond(Object.assign({ returnValue: true }, trakt.status())));
register('traktLink', async (msg) => {
  try { msg.respond(Object.assign({ returnValue: true }, await trakt.startDevice())); } catch (e) { fail(msg, e.message); }
});
register('traktDisconnect', (msg) => msg.respond(Object.assign({ returnValue: true }, trakt.disconnect())));

service.register('ping', (msg) => {
  msg.respond({ returnValue: true, version: pkg.version, uid: process.getuid ? process.getuid() : null, imagePort: IMAGE_PORT });
});

// ------------------------------------------------------------------ Stremio

let cache = { at: 0, items: null };

const META = 'https://images.metahub.space';

function parseVideoId(videoId) {
  // series episodes look like "tt0903747:2:5"; anime ids like "kitsu:42189:1" are show:episode, no season
  const m = /^(.+):(\d+):(\d+)$/.exec(videoId || '');
  if (!m) return { season: 0, episode: 0 };
  if (/^(kitsu|mal|anilist|anidb)$/.test(m[1])) return { season: 0, episode: +m[3] };
  return { season: +m[2], episode: +m[3] };
}

// Stremio ids keep their colons in deep links (tt0903747:2:5).
const enc = (s) => encodeURIComponent(s).replace(/%3A/gi, ':');

function toItem(li) {
  const st = li.state || {};
  const id = li._id;
  const imdb = /^tt\d+$/.test(id);
  const videoId = st.video_id || (li.type === 'movie' ? id : '');
  const ep = parseVideoId(videoId);
  const duration = st.duration || 0;
  const offset = st.timeOffset || 0;
  // Full-screen hero gets the large artwork; the small cards only need medium.
  const background = li.background || (imdb ? `${META}/background/large/${id}/img` : '');
  const thumbnail = imdb ? `${META}/background/medium/${id}/img` : (li.background || li.poster || '');
  return {
    id,
    type: li.type,
    name: li.name,
    poster: li.poster || '',
    background,
    thumbnail,
    logo: li.logo || (imdb ? `${META}/logo/medium/${id}/img` : ''),
    season: ep.season,
    episode: ep.episode,
    progress: duration ? Math.min(1, offset / duration) : 0,
    remainingMin: duration ? Math.max(1, Math.round((duration - offset) / 60000)) : 0,
    lastWatched: st.lastWatched || li._mtime || '',
    deepLink: `stremio:///detail/${enc(li.type)}/${enc(id)}` + (videoId ? '/' + enc(videoId) : '')
  };
}

async function continueWatching(authKey) {
  const lib = await stremio('datastoreGet', { authKey, collection: 'libraryItem', ids: [], all: true });
  return sortStable((lib || []).filter((li) => (!li.removed || li.temp) && li.state && li.state.timeOffset > 0),
    (a, b) => String(b.state.lastWatched || '').localeCompare(String(a.state.lastWatched || '')))
    .slice(0, 15)
    .map(toItem);
}

// ---- Stremio sign-in on the TV, like Stremio's own TV apps: a code and a QR
// code for link.stremio.com; you approve it on your phone, signed in to Stremio.
const STREMIO_LINK = process.env.HOMEPLUS_STREMIO_LINK || 'https://link.stremio.com';
const stremioLink = { pending: null, error: '', timer: null };

function stremioStatus() {
  const cfg = readConfig();
  const p = stremioLink.pending;
  if (p && Date.now() > p.expires_at) stremioLink.pending = null;
  return {
    connected: !!cfg.stremioAuthKey,
    user: cfg.stremioEmail || '',
    pending: stremioLink.pending ? { code: p.code, url: 'link.stremio.com', qr: p.qrcode } : null, // the code goes in there
    error: stremioLink.error
  };
}

async function stremioStartLink() {
  stremioLink.error = '';
  const r = await requestJSON('GET', `${STREMIO_LINK}/api/v2/create?type=Create`);
  const res = r.json && r.json.result;
  if (r.status !== 200 || !res || !res.code) {
    stremioLink.error = 'Stremio didn\u2019t give a sign-in code. Try again.';
    return stremioStatus();
  }
  stremioLink.pending = { code: res.code, link: res.link, qrcode: res.qrcode, expires_at: Date.now() + 10 * 60 * 1000 };
  log('stremio: link code issued');
  clearTimeout(stremioLink.timer);
  const poll = async () => {
    const p = stremioLink.pending;
    if (!p || Date.now() > p.expires_at) { stremioLink.pending = null; return; }
    try {
      const c = await requestJSON('GET', `${STREMIO_LINK}/api/v2/read?type=Read&code=${encodeURIComponent(p.code)}`);
      const key = c.json && c.json.result && c.json.result.authKey;
      if (key) {
        const cfg = readConfig();
        cfg.stremioAuthKey = key;
        writeConfig(cfg);
        cache = { at: 0, items: null };
        stremioLink.pending = null;
        log('stremio: signed in with a code');
        return;
      }
    } catch (e) {
      log('stremio: link poll error', e.message);
    }
    stremioLink.timer = setTimeout(poll, 3000);
  };
  stremioLink.timer = setTimeout(poll, 3000);
  return stremioStatus();
}

async function stremioSignOut() {
  const cfg = readConfig();
  if (cfg.stremioAuthKey) {
    try { await stremio('logout', { type: 'Logout', authKey: cfg.stremioAuthKey }); } catch (e) { /* best effort */ }
  }
  delete cfg.stremioAuthKey;
  delete cfg.stremioEmail;
  writeConfig(cfg);
  cache = { at: 0, items: null };
  stremioLink.pending = null;
  return stremioStatus();
}

register('stremioStatus', (msg) => {
  const cfg = readConfig();
  msg.respond({ returnValue: true, connected: !!cfg.stremioAuthKey, email: cfg.stremioEmail || '' });
});

register('stremioLogin', async (msg) => {
  const { email, password } = msg.payload;
  if (!email || !password) return fail(msg, 'email and password are required');
  try {
    const r = await stremio('login', { type: 'Login', email, password, facebook: false });
    if (!r || !r.authKey) throw new Error('No auth key returned');
    const cfg = readConfig();
    cfg.stremioAuthKey = r.authKey;
    cfg.stremioEmail = email;
    writeConfig(cfg);
    cache = { at: 0, items: null };
    msg.respond({ returnValue: true, connected: true });
  } catch (e) {
    fail(msg, e.message);
  }
});

register('stremioLogout', async (msg) => {
  await stremioSignOut();
  msg.respond({ returnValue: true, connected: false });
});

// A Jellyfin server Home+ can't find by itself (another network, a reverse proxy); "" forgets it.
register('jellyfinServer', (msg) => {
  try {
    msg.respond(Object.assign({ returnValue: true }, media.jellyfin.setServer(msg.payload.url)));
  } catch (e) {
    fail(msg, e.message);
  }
});

