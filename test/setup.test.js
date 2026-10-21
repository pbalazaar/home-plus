/* What Home+ sets up on the TV outside its own folders, and that it's harmless once Home+ is gone,
 * as the Homebrew Channel's rules ask: the boot script, linked (not copied) into the Homebrew
 * Channel's init.d, and LG Input Hook's binds for the Home and Settings keys, which do what the
 * keys do without Home+ once it's gone. Then Remove Home+, which takes it all away. The real
 * service, on a stand-in TV that Home+ 0.11 had set up. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { startService, until } = require('./helpers');

const ROOT = path.join(__dirname, '..');
const SCRIPT = path.join(ROOT, 'service', 'settings-key.sh'); /* where the service is, its script is */
const windows = process.platform === 'win32' && 'needs symlinks and sh'; /* a reason to skip */
let svc, dir, app, initd, bindsFile;
const binds = () => JSON.parse(fs.readFileSync(bindsFile, 'utf8'));
const theirs = { 1: { action: 'launch', id: 'netflix' }, 2: { action: 'exec', command: 'echo hi' } };

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-setup-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  app = path.join(dir, 'applications', 'com.github.pmiraka.homeplus'); /* Home+, installed */
  fs.mkdirSync(path.join(app, 'init.d'), { recursive: true });
  fs.copyFileSync(path.join(ROOT, 'native', 'appinfo.json'), path.join(app, 'appinfo.json'));
  fs.copyFileSync(path.join(ROOT, 'init.d', '50-homeplus'), path.join(app, 'init.d', '50-homeplus'));
  initd = path.join(dir, 'webosbrew', 'init.d'); /* with the copy of the boot script 0.11 made, and its log */
  fs.mkdirSync(initd, { recursive: true });
  fs.writeFileSync(path.join(initd, '50-homeplus'), '#!/bin/sh\n# Home+ 0.11\n', { mode: 0o755 });
  fs.writeFileSync(path.join(initd, '90-someone-else'), '#!/bin/sh\n', { mode: 0o755 });
  fs.writeFileSync(path.join(dir, 'webosbrew', 'homeplus-boot.log'), 'homeplus boot\n');
  bindsFile = path.join(dir, 'lginputhook', 'keybinds.json');
  fs.mkdirSync(path.dirname(bindsFile));
  fs.writeFileSync(bindsFile, JSON.stringify(Object.assign({
    773: { action: 'launch', id: 'com.github.pmiraka.homeplus' }, /* made in LG Input Hook's app, as the README said */
    139: { action: 'exec', command: `sh ${SCRIPT}` } /* made by Home+ 0.11 */
  }, theirs)));
  svc = await startService(dir, { HOMEPLUS_APP_DIR: app, HOMEPLUS_INITD_DIR: initd, HOMEPLUS_INPUTHOOK_BINDS: bindsFile });
});

test.after(async () => {
  if (svc) await svc.stop();
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

test('setup: the boot script is a link to the one in the package, in place of the old copy', { skip: windows }, async () => {
  const link = path.join(initd, '50-homeplus'), script = path.join(app, 'init.d', '50-homeplus');
  const linked = () => { try { return fs.readlinkSync(link) === script; } catch (e) { return false; } };
  await until(linked, 8000, 'the boot script to be linked');
  assert.ok(fs.statSync(link).mode & 0o100, 'run-parts runs only what it can execute');
  assert.equal((await svc.call('GET', '/api/status', { auth: false })).json.bootScript, true);
  assert.equal(fs.readFileSync(path.join(initd, '90-someone-else'), 'utf8'), '#!/bin/sh\n', 'other boot scripts stay');
});

test('setup: Home+\'s binds from before turn into ones that check for Home+, and everyone else\'s stay', async () => {
  const b = binds(), dirOf = path.dirname(bindsFile);
  /* plain commands (LG Input Hook runs a bind without a shell), each a script beside the binds */
  assert.deepEqual(b[773], { action: 'exec', command: `sh ${path.join(dirOf, 'homeplus-home.sh')}` });
  assert.deepEqual(b[139], { action: 'exec', command: `sh ${path.join(dirOf, 'homeplus-settings.sh')}` });
  assert.match(fs.readFileSync(path.join(dirOf, 'homeplus-home.sh'), 'utf8'), /com\.github\.pmiraka\.homeplus.+\n.*com\.webos\.app\.home"/s);
  assert.match(fs.readFileSync(path.join(dirOf, 'homeplus-settings.sh'), 'utf8'), /settings-key\.sh.+\n.*com\.webos\.app\.quicksettings"/s);
  assert.deepEqual({ 1: b[1], 2: b[2] }, theirs);
  assert.deepEqual((await svc.call('GET', '/api/home')).json.settingsKey, { available: true, on: true });
});

test('setup: the keys do what they do without Home+ once it\'s gone', { skip: windows }, () => {
  /* each bind run as LG Input Hook runs it, as a plain command (no shell), with a luna-send that notes its call */
  const bin = path.join(dir, 'bin'), calls = path.join(dir, 'calls');
  fs.mkdirSync(bin, { recursive: true });
  fs.writeFileSync(path.join(bin, 'luna-send'), '#!/bin/sh\necho "luna-send $*" >>"$CALLS"\n', { mode: 0o755 });
  const press = (command) => {
    fs.writeFileSync(calls, '');
    const [prog, ...args] = command.split(' ');
    const r = spawnSync(prog, args, { env: Object.assign({}, process.env, { PATH: `${bin}${path.delimiter}${process.env.PATH}`, CALLS: calls }) });
    assert.equal(r.status, 0, String(r.stderr));
    return fs.readFileSync(calls, 'utf8').trim();
  };
  const { 773: home, 139: settings } = binds();
  const appinfo = path.join(app, 'appinfo.json'), away = path.join(dir, 'appinfo.away');

  assert.equal(press(home.command), 'luna-send -n 1 luna://com.webos.applicationManager/launch {"id":"com.github.pmiraka.homeplus"}');
  fs.renameSync(appinfo, away); /* Home+, removed */
  try {
    assert.equal(press(home.command), 'luna-send -n 1 luna://com.webos.applicationManager/launch {"id":"com.webos.app.home"}', 'LG\'s home screen');
  } finally {
    fs.renameSync(away, appinfo);
  }

  /* the Settings script, pointing at a stand-in for settings-key.sh that notes it ran, then at one that's gone */
  const script = path.join(path.dirname(bindsFile), 'homeplus-settings.sh'), text = fs.readFileSync(script, 'utf8');
  const stand = path.join(dir, 'settings-key.sh');
  fs.writeFileSync(stand, 'echo "settings-key.sh ran" >>"$CALLS"\n');
  try {
    fs.writeFileSync(script, text.split(SCRIPT).join(stand));
    assert.equal(press(settings.command), 'settings-key.sh ran');
    fs.writeFileSync(script, text.split(SCRIPT).join(path.join(dir, 'removed', 'settings-key.sh')));
    assert.equal(press(settings.command),
      'luna-send -n 1 luna://com.webos.service.systemUIManager/launch {"appId":"com.webos.app.quicksettings"}', 'LG\'s Quick Settings');
  } finally {
    fs.writeFileSync(script, text);
  }
});

test('setup: a Home bind made later in LG Input Hook\'s app gets the same check at once', async () => {
  const upgraded = binds()[773];
  fs.writeFileSync(bindsFile, JSON.stringify(Object.assign(binds(), { 773: { action: 'launch', id: 'com.github.pmiraka.homeplus' } }), null, 2));
  await until(() => binds()[773].action === 'exec', 8000, 'the Home bind to be upgraded');
  assert.deepEqual(binds()[773], upgraded);
  assert.deepEqual({ 1: binds()[1], 2: binds()[2] }, theirs);
});

/* last: it removes Home+ */
test('setup: Remove Home+ takes away its boot script and log, its binds and its settings, and nothing else', async () => {
  const copy = () => { try { return fs.lstatSync(path.join(initd, '50-homeplus')).isFile(); } catch (e) { return false; } };
  await until(() => !copy(), 8000, 'the service to see to the boot script'); /* 2 s after it starts */
  const r = await svc.call('POST', '/api/uninstall');
  assert.equal(r.status, 200);
  assert.equal(r.json.bootScript, true);
  assert.equal(r.json.inputhookBinds, 2);
  assert.throws(() => fs.lstatSync(path.join(initd, '50-homeplus')), /ENOENT/, 'the boot script');
  assert.equal(fs.existsSync(path.join(dir, 'webosbrew', 'homeplus-boot.log')), false, 'its log');
  assert.ok(fs.existsSync(path.join(initd, '90-someone-else')), 'other boot scripts stay');
  assert.deepEqual(binds(), theirs);
  for (const f of ['homeplus-home.sh', 'homeplus-settings.sh'])
    assert.equal(fs.existsSync(path.join(path.dirname(bindsFile), f)), false, `the binds' ${f}`);
  assert.equal(fs.existsSync(path.join(dir, 'config')), false, 'settings, sign-ins and pictures');
});
