/* The service's local HTTP API, end to end: the real service.js on the preview harness's
 * stand-in webOS (tools/native-preview/harness.js), with its mock Stremio library and TMDB.
 * It runs on its own port with its config in a temporary folder, so no root is needed.
 * HOMEPLUS_SERVICE_NODE runs it with another Node, including webOS 22/23's Node 12 (CI does). */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { mockServer, startService, until } = require('./helpers');

const ROOT = path.join(__dirname, '..');
let svc, dir;
const call = (...args) => svc.call(...args);
const within5s = (p) => Promise.race([p, new Promise((resolve, reject) => setTimeout(() => reject(new Error('still waiting after 5 s')), 5000))]);

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-api-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix,youtube|YouTube,stremio|Stremio,com.github.pmiraka.homeplus|Home+'); /* (and the old Home+, not uninstalled yet) */
  for (let i = 0; i < 5; i++) fs.writeFileSync(path.join(dir, `show${i}.jpg`), `a picture of show ${i}`); /* the mock Stremio's */
  fs.mkdirSync(path.join(dir, 'lginputhook')); /* LG Input Hook, with the Home key bound to Home+ */
  fs.writeFileSync(path.join(dir, 'lginputhook', 'keybinds.json'), JSON.stringify({ 773: { action: 'launch', id: 'com.github.pbalazaar.homeplus' } }));
  svc = await startService(dir, {
    HOMEPLUS_PREVIEW_TMDB: '1',
    HOMEPLUS_PREVIEW_FOREGROUND: 'com.github.pbalazaar.homeplus', /* Home+ is in front */
    HOMEPLUS_INPUTHOOK_BINDS: path.join(dir, 'lginputhook', 'keybinds.json')
  });
});

test.after(async () => {
  if (svc) await svc.stop();
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

test('api: /api/status answers without the token (the launcher asks before it has one)', async () => {
  const r = await call('GET', '/api/status', { auth: false });
  assert.equal(r.status, 200);
  assert.equal(r.json.version, JSON.parse(fs.readFileSync(path.join(ROOT, 'service/package.json'), 'utf8')).version);
  assert.equal(r.json.token, true, 'the token file was written');
  assert.equal(r.json.id, 'com.github.pbalazaar.homeplus.service', 'which service this is: the old Home+\'s answers on the same port');
  assert.equal(r.json.tmdbConfigured, true, 'the first status response lets the launcher load provider titles immediately');
});

test('api: the welcome steps: done once finished or skipped; an install that answered the Home question has had them', async () => {
  const onboarded = async () => (await call('GET', '/api/status', { auth: false })).json.onboarded;
  assert.equal(await onboarded(), true, 'Home+ before the welcome steps asked only about the Home button, and it was answered');
  await call('POST', '/api/home', { body: { onboarded: false } }); /* they start (as on a fresh install) */
  assert.equal(await onboarded(), false);
  const r = await call('POST', '/api/home', { body: { mode: 'watch', asked: true, onboarded: false } }); /* the Home step */
  assert.equal(r.json.mode, 'watch');
  assert.equal(await onboarded(), false, 'answering the Home question halfway through isn\'t finishing');
  await call('POST', '/api/home', { body: { onboarded: true } });
  assert.equal(await onboarded(), true);
  assert.equal(JSON.parse(fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8')).onboarded, true, 'kept in config.json');
});

test('api: everything else needs the token', async () => {
  assert.equal((await call('GET', '/api/apps', { auth: false })).status, 403);
  assert.equal((await call('GET', '/api/apps', { headers: { 'X-Homeplus-Token': 'wrong' } })).status, 403);
  const r = await call('GET', '/api/apps');
  assert.equal(r.status, 200);
  assert.deepEqual(r.json.apps.map((a) => a.id).sort(), ['netflix', 'stremio', 'youtube']);
});

test('api: web pages and other hosts are turned away, even with the token', async () => {
  assert.equal((await call('GET', '/api/apps', { headers: { Origin: 'http://example.com' } })).status, 403);
  assert.equal((await call('GET', '/api/status', { auth: false, headers: { Origin: 'http://example.com' } })).status, 403);
  assert.equal((await call('GET', '/api/apps', { headers: { Host: 'evil.example.com' } })).status, 403, 'DNS rebinding');
});

test('api: Continue Watching from Stremio, with every account\'s state for Settings', async () => {
  const r = await call('GET', '/api/continue');
  assert.equal(r.status, 200);
  assert.equal(r.json.source, 'stremio');
  assert.ok(r.json.items.length > 0);
  assert.ok(r.json.items.every((it) => it.source === 'stremio' && it.name));
  assert.ok(r.json.items.every((it) => !it.background || it.background.startsWith('/img/')), 'pictures go through the service');
  assert.ok(r.json.items.every((it) => it.genre && it.year && it.rating && it.overview), 'Stremio titles include TMDB details');
  assert.equal(r.json.tmdb.configured, true);
  assert.equal(typeof r.json.jellyfin.connected, 'boolean');
  assert.equal(typeof r.json.plex.connected, 'boolean');
});

test('api: Top Shelf detail preference keeps the original Stremio list when off', async () => {
  try {
    const off = await call('POST', '/api/prefs', { body: { topShelfDetails: 'off' } });
    assert.equal(off.json.topShelfDetails, 'off');
    const plain = (await call('GET', '/api/continue')).json.items.filter((it) => it.source === 'stremio');
    assert.ok(plain.length > 0);
    assert.ok(plain.every((it) => !it.genre && !it.rating && !it.overview));

    const compact = await call('POST', '/api/prefs', { body: { topShelfDetails: 'compact' } });
    assert.equal(compact.json.topShelfDetails, 'compact');
    const enriched = (await call('GET', '/api/continue')).json.items.filter((it) => it.source === 'stremio');
    assert.ok(enriched.every((it) => it.genre && it.rating && it.overview));
  } finally {
    await call('POST', '/api/prefs', { body: { topShelfDetails: 'full' } });
  }
  assert.equal((await call('GET', '/api/prefs')).json.topShelfDetails, 'full');
});

test('api: what\'s popular on a streaming app, and what\'s trending for an app with no picture', async () => {
  const netflix = await call('GET', '/api/featured?app=netflix');
  assert.equal(netflix.json.kind, 'popular');
  assert.equal(netflix.json.provider, 'Netflix');
  assert.ok(netflix.json.items.length >= 3);
  assert.ok(netflix.json.items.every((it) => it.background.startsWith('/img/')));
  assert.equal((await call('GET', '/api/featured?app=youtube')).json.kind, 'trending');
  assert.equal((await call('GET', '/api/featured?app=nothing.installed')).json.items.length, 0);
});

test('api: a title\'s picture comes through the service, and is kept for next time', async () => {
  const pic = (await call('GET', '/api/continue')).json.items.find((it) => it.background).background;
  const get = () => new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: svc.port, path: pic, headers: { 'X-Homeplus-Token': svc.token } }, (res) => {
      let body = '';
      res.on('data', (c) => { body += c; });
      res.on('end', () => resolve({ status: res.statusCode, body }));
    }).on('error', reject);
  });
  const first = await get();
  assert.equal(first.status, 200);
  assert.match(first.body, /^a picture of show \d$/);
  const kept = path.join(dir, 'config', 'images', pic.slice('/img/'.length));
  await until(() => fs.existsSync(kept), 5000, 'the picture to be kept');
  assert.equal(fs.readFileSync(kept, 'utf8'), first.body);
  assert.deepEqual(await get(), first);
});

test('api: Recents lists the apps you opened that still run, and a closed one stays out', async () => {
  /* the stand-in TV runs Netflix, YouTube, Spotify and Disney+; YouTube, then Netflix, then
   * Spotify were opened (Spotify isn't on this home screen); Disney+ never was */
  const ids = async () => (await call('GET', '/api/running')).json.apps.map((a) => a.id);
  assert.deepEqual(await ids(), ['youtube', 'netflix']);
  assert.equal((await call('POST', '/api/close', { body: { id: 'youtube' } })).status, 200);
  assert.deepEqual(await ids(), ['netflix'], 'webOS still runs YouTube (it may start it again), but you closed it');
});

test('api: the Control Center reads the TV and changes it', async () => {
  const st = await call('GET', '/api/control');
  assert.equal(st.status, 200);
  assert.deepEqual(st.json.network, { kind: 'wifi', name: 'Home Wi-Fi' });
  assert.ok(st.json.inputs.some((i) => i.device === 'PS5'));
  const before = st.json.sleepTimer;
  const after = await call('POST', '/api/control', { body: { action: 'sleepTimer', value: 'next' } });
  assert.equal(after.status, 200);
  assert.notEqual(after.json.sleepTimer, before);
  const bad = await call('POST', '/api/control', { body: { action: 'sleepTimer', value: '45' } });
  assert.ok(bad.status >= 400, 'a value LG doesn\'t have is refused');
  assert.match(bad.json.errorText, /sleep timer/);
  assert.equal((await call('GET', '/api/control')).json.sleepTimer, after.json.sleepTimer, 'and nothing changed');
});

test('api: preferences are kept in the config file', async () => {
  await call('POST', '/api/prefs', { body: { clock24: true } });
  assert.equal((await call('GET', '/api/prefs')).json.clock24, true);
  const cfg = JSON.parse(fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8'));
  assert.equal(cfg.prefs.clock24, true);
});

test('api: sign-ins change things, so they take POST only', async () => {
  assert.equal((await call('GET', '/api/stremio/link')).status, 405);
  assert.equal((await call('GET', '/api/stremio/status')).status, 200);
});

test('api: the Settings key binds through LG Input Hook, and only Home+\'s bind comes and goes', async () => {
  const binds = () => JSON.parse(fs.readFileSync(path.join(dir, 'lginputhook', 'keybinds.json'), 'utf8'));
  const home = binds()[773];
  assert.deepEqual((await call('GET', '/api/home')).json.settingsKey, { available: true, on: false });
  const on = await call('POST', '/api/home', { body: { settingsKey: true } });
  assert.deepEqual(on.json.settingsKey, { available: true, on: true });
  assert.equal(binds()[139].action, 'exec');
  const script = path.join(dir, 'lginputhook', 'homeplus-settings.sh'); /* a plain command: LG Input Hook runs it without a shell */
  assert.equal(binds()[139].command, `sh ${script}`);
  assert.match(fs.readFileSync(script, 'utf8'), /settings-key\.sh\n.*com\.webos\.app\.quicksettings/, 'LG\'s Quick Settings once Home+ is gone');
  assert.deepEqual(binds()[773], home, 'the Home key\'s bind stays');
  await call('POST', '/api/home', { body: { settingsKey: false } });
  assert.equal(binds()[139], undefined);
  assert.deepEqual(binds()[773], home);
});

test('api: the Input key binds through LG Input Hook too, beside the Settings key\'s', async () => {
  const binds = () => JSON.parse(fs.readFileSync(path.join(dir, 'lginputhook', 'keybinds.json'), 'utf8'));
  const home = binds()[773];
  assert.deepEqual((await call('GET', '/api/home')).json.inputKey, { available: true, on: false });
  await call('POST', '/api/home', { body: { settingsKey: true } });
  const on = await call('POST', '/api/home', { body: { inputKey: true } });
  assert.deepEqual(on.json.inputKey, { available: true, on: true });
  const script = path.join(dir, 'lginputhook', 'homeplus-input.sh'); /* a plain command: LG Input Hook runs it without a shell */
  assert.deepEqual(binds()[241], { action: 'exec', command: `sh ${script}` });
  assert.match(fs.readFileSync(script, 'utf8'), /input-key\.sh\n.*com\.webos\.app\.quickinputpicker/, 'LG\'s Input picker once Home+ is gone');
  assert.equal(binds()[139].command, `sh ${path.join(dir, 'lginputhook', 'homeplus-settings.sh')}`, 'the Settings key\'s stays');
  await call('POST', '/api/home', { body: { inputKey: false } });
  assert.equal(binds()[241], undefined);
  assert.equal(binds()[139].action, 'exec', 'and still does');
  await call('POST', '/api/home', { body: { settingsKey: false } });
  assert.deepEqual(binds()[773], home);
});

test('api: the Input key, with Home+ in front, reaches the launcher as an event', async () => {
  const waiting = call('GET', '/api/events');
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.deepEqual((await call('POST', '/api/key', { body: { key: 'input' } })).json, { opened: 'controlCenter' });
  assert.deepEqual((await waiting).json, { events: ['inputs'] });
});

test('api: the Settings key, with Home+ in front, reaches the launcher as an event', async () => {
  const waiting = call('GET', '/api/events'); /* the launcher's request, answered when something happens */
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.deepEqual((await call('POST', '/api/key', { body: { key: 'settings' } })).json, { opened: 'controlCenter' });
  assert.deepEqual((await waiting).json, { events: ['controlCenter'] });
  assert.ok((await call('POST', '/api/key', { body: { key: 'volumeUp' } })).status >= 400, 'only keys Home+ handles');
});

test('api: an app can be deleted from the TV, but not Home+ itself or what roots the TV', async () => {
  const apps = (await call('GET', '/api/apps')).json.apps;
  assert.equal(apps.find((a) => a.id === 'youtube').removable, true);
  assert.deepEqual((await call('POST', '/api/remove', { body: { id: 'youtube' } })).json, { returnValue: true, deleted: 'youtube' });
  const self = await call('POST', '/api/remove', { body: { id: 'com.github.pbalazaar.homeplus' } });
  assert.ok(self.status >= 400);
  assert.match(self.json.errorText, /can't be deleted/);
  assert.ok((await call('POST', '/api/remove', { body: { id: 'not.installed' } })).status >= 400);
});

test('api: a TV service that never answers doesn\'t keep a request waiting', async () => {
  /* a second service, on a TV whose app list and network state never come (a luna call gives up
   * after 15 s on the TV; here, after 500 ms) */
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-api-silent-'));
  fs.writeFileSync(path.join(d, 'apps.txt'), 'netflix|Netflix');
  const s = await startService(d, { HOMEPLUS_PREVIEW_LUNA_SILENT: 'listApps|connectionmanager', HOMEPLUS_LUNA_TIMEOUT_MS: '500' });
  try {
    const apps = await within5s(s.call('GET', '/api/apps'));
    assert.ok(apps.status >= 500, `no app list is an error, not an empty list: ${apps.status}`);
    assert.match(apps.json.errorText, /didn't list its apps/);
    const cc = await within5s(s.call('GET', '/api/control'));
    assert.equal(cc.status, 200, 'the Control Center still opens, with what did answer');
    assert.equal(cc.json.network.kind, 'none');
  } finally {
    await s.stop();
    fs.rmSync(d, { recursive: true, force: true, maxRetries: 5 });
  }
});

test('api: a server that hangs up halfway through its reply doesn\'t keep Continue Watching waiting', async () => {
  /* a second service, whose Stremio, Trakt and Jellyfin all hang up halfway through every reply: on
   * Node 16 and newer such a reply never ended, and nor did the request for the shelf */
  const cut = await mockServer(() => ({ cut: '{"result": [' }));
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-api-cut-'));
  fs.writeFileSync(path.join(d, 'apps.txt'), 'stremio|Stremio');
  const s = await startService(d, { HOMEPLUS_PREVIEW_HTTPS: cut.base }); /* Stremio's and Trakt's APIs */
  try {
    const file = path.join(d, 'config', 'config.json');
    fs.writeFileSync(file, JSON.stringify(Object.assign(JSON.parse(fs.readFileSync(file, 'utf8')), {
      traktClientId: 'id', traktAccess: 'token', traktExpires: Date.now() + 86400000,
      jellyfin: { url: cut.base, userId: 'u1', user: 'Ann', token: 'token' }
    })));
    for (const p of ['/api/continue', '/api/continue?source=jellyfin']) {
      const r = await within5s(s.call('GET', p));
      assert.equal(r.status, 200, p);
      assert.deepEqual(r.json.items, [], p);
    }
    for (const p of ['/api/datastoreGet', '/sync/playback', '/UserItems/Resume']) assert.ok(cut.hits.some((h) => h.path === p), `asked for ${p}`);
  } finally {
    await s.stop();
    await cut.close();
    fs.rmSync(d, { recursive: true, force: true, maxRetries: 5 });
  }
});

test('api: the Control Center\'s list and gallery, saved, stepped and reset', async () => {
  const st = (await call('GET', '/api/control')).json;
  assert.equal(st.controls.length, 11, 'the default list');
  assert.equal(st.controls[0].id, 'power');
  assert.ok(st.catalog.find((e) => e.id === 'soundMode').available);
  assert.equal(st.catalog.find((e) => e.id === 'ambientLight').available, false, 'no HyperHDR on the stand-in TV');
  const saved = await call('POST', '/api/control', { body: { action: 'layout', controls: [{ id: 'soundMode', size: 'wide' }, { id: 'power', size: 'small' }] } });
  assert.deepEqual(saved.json.controls.map((c) => `${c.id}:${c.size}:${c.value}`), ['soundMode:wide:Cinema', 'power:small:', 'settings:small:']);
  const ran = await call('POST', '/api/control', { body: { action: 'run', value: 'soundMode' } });
  assert.equal(ran.json.controls[0].value, 'Clear Voice', 'the next sound mode');
  const reset = await call('POST', '/api/control', { body: { action: 'reset' } });
  assert.equal(reset.json.controls.length, 11);
  assert.ok((await call('POST', '/api/control', { body: { action: 'run', value: 'nothing' } })).status >= 400);
});
