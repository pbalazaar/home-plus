/* OK on a Jellyfin card, end to end: POST /api/resume on the real service, on the preview's stand-in webOS, with a
 * stand-in Jellyfin. The session rule itself is in jellyfin.test.js; here: what opens, what the toasts say, and who
 * decides a run is over. Each test has a service of its own, so they run at once. */
'use strict';

const { describe, test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { mockServer, startService, sleep, until } = require('./helpers');

const MIN = 600000000;
const JF = 'org.jellyfin.webos';
const MOVIE = { Id: 'm1', Type: 'Movie', Name: 'Film', RunTimeTicks: 100 * MIN, UserData: { PlaybackPositionTicks: 25 * MIN } };
const NEXT = { Id: 'e2', Type: 'Episode', Name: 'Next', SeriesName: 'Show', ParentIndexNumber: 1, IndexNumber: 3, RunTimeTicks: 50 * MIN, UserData: { PlaybackPositionTicks: 0 } };
const AT = '192.168.1.50';
const HOME = { Id: 'home', DeviceId: 'dev1', DeviceName: 'LG TV', RemoteEndPoint: AT, SupportsRemoteControl: false };
const TV = { Id: 'tv', DeviceId: 'app', DeviceName: 'LG Smart TV', Client: 'Jellyfin Web', RemoteEndPoint: AT, SupportsRemoteControl: true };
const TODAY = '“Film” is in Jellyfin’s Continue Watching';

/* the service, signed in to a stand-in Jellyfin; state: what that says (connected: the TV's app has its session;
 * broken: the session list fails) */
async function start(env = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-resume-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), `${JF}|Jellyfin,youtube|YouTube,broken|Broken`);
  const state = { connected: true, broken: false, unregistered: false, playing: null };
  const server = await mockServer((req, url) => {
    if (url.pathname === '/UserItems/Resume') return { json: { Items: [MOVIE] } };
    if (url.pathname === '/Shows/NextUp') return { json: { Items: [NEXT] } };
    if (url.pathname === '/Sessions/Capabilities') return { status: state.unregistered ? 500 : 204, body: '' };
    if (url.pathname === '/Sessions') {
      if (state.broken) return { status: 500, json: {} };
      return { json: [HOME].concat(state.connected ? [Object.assign({ NowPlayingItem: state.playing }, TV)] : []) };
    }
    if (url.pathname === '/Sessions/tv/Playing') {
      state.playing = { Id: url.searchParams.get('itemIds') };
      return { status: 204, body: '' };
    }
    return null;
  });
  const luna = path.join(dir, 'luna.log');
  const svc = await startService(dir, Object.assign({
    HOMEPLUS_PREVIEW_LUNA_LOG: luna,
    HOMEPLUS_PREVIEW_FOREGROUND: 'com.github.pbalazaar.homeplus', /* Home+ is in front */
    HOMEPLUS_PREVIEW_LAUNCH_REFUSED: 'broken'
  }, env));
  const file = path.join(dir, 'config', 'config.json');
  const cfg = JSON.parse(fs.readFileSync(file, 'utf8'));
  cfg.jellyfin = { url: server.base, name: 'Home', userId: 'u1', user: 'Ann', token: 'tok' };
  cfg.jellyfinDevice = 'dev1';
  fs.writeFileSync(file, JSON.stringify(cfg));
  const lines = () => (fs.existsSync(luna) ? fs.readFileSync(luna, 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l)) : []);
  return {
    state,
    resume: (body) => svc.call('POST', '/api/resume', { body: Object.assign({ app: JF, source: 'jellyfin' }, body) }),
    launch: (body) => svc.call('POST', '/api/launch', { body }),
    launches: () => lines().filter((l) => l.uri === 'luna://com.webos.applicationManager/launch').map((l) => l.params.id),
    toasts: () => lines().filter((l) => l.uri.endsWith('/createToast')).map((l) => l.params.message),
    sent: () => server.hits.filter((h) => /\/Playing$/.test(h.path)),
    asked: (p) => server.hits.filter((h) => h.path === p),
    async stop() {
      await svc.stop();
      await server.close();
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
    }
  };
}

async function withService(env, run) {
  const t = await start(env);
  try {
    await run(t);
  } finally {
    await t.stop();
  }
}

describe('OK on a Jellyfin card', { concurrency: true }, () => {
  test('a title that isn\'t in the list: refused, and nothing opens', () => withService({}, async (t) => {
    const r = await t.resume({ id: 'nope' });
    assert.equal(r.status, 404);
    assert.equal(r.json.returnValue, false);
    assert.deepEqual(t.launches(), []);
  }));

  test('a source that can\'t play, or no app: refused', () => withService({}, async (t) => {
    assert.equal((await t.resume({ source: 'plex', id: 'm1' })).status, 400);
    assert.equal((await t.resume({ source: 'constructor', id: 'm1' })).status, 400);
    assert.equal((await t.resume({ app: '', id: 'm1' })).status, 400);
    assert.deepEqual(t.launches(), []);
  }));

  test('a title you\'ve started: the app opens, then it plays from where it stopped, with "Resuming"', () => withService({}, async (t) => {
    t.state.connected = false; /* the app is still starting: the answer doesn't wait for it */
    const r = await t.resume({ id: 'm1' });
    assert.equal(r.status, 200);
    assert.equal(r.json.returnValue, true);
    assert.deepEqual(t.launches(), [JF]);
    assert.equal(t.sent().length, 0);
    t.state.connected = true;
    await until(() => t.sent().length, 5000, 'PlayNow');
    assert.equal(t.sent()[0].query.get('startPositionTicks'), String(25 * MIN));
    await until(() => t.toasts().length, 5000, 'the toast');
    assert.deepEqual(t.toasts(), ['Resuming “Film”']);
  }));

  test('a next episode: it plays from its start, with "Playing"', () => withService({}, async (t) => {
    assert.equal((await t.resume({ id: 'e2' })).status, 200);
    await until(() => t.sent().length, 5000, 'PlayNow');
    assert.equal(t.sent()[0].query.get('startPositionTicks'), '0');
    await until(() => t.toasts().length, 5000, 'the toast');
    assert.deepEqual(t.toasts(), ['Playing “Show”']);
  }));

  test('Play From Beginning: from its start, with "Playing"', () => withService({}, async (t) => {
    assert.equal((await t.resume({ id: 'm1', fromStart: true })).status, 200);
    await until(() => t.sent().length, 5000, 'PlayNow');
    assert.equal(t.sent()[0].query.get('startPositionTicks'), '0');
    await until(() => t.toasts().length, 5000, 'the toast');
    assert.deepEqual(t.toasts(), ['Playing “Film”']);
  }));

  test('the server failing before the toast shows: today\'s toast in its place', () => withService({}, async (t) => {
    t.state.unregistered = true;
    assert.equal((await t.resume({ id: 'm1' })).status, 200);
    await until(() => t.toasts().length, 5000, 'the toast');
    await sleep(300);
    assert.deepEqual(t.toasts(), [TODAY], 'not "Resuming", then nothing');
    assert.deepEqual(t.launches(), [JF], 'the app stays open');
  }));

  test('the server failing after it: today\'s toast follows it', () => withService({}, async (t) => {
    t.state.connected = false;
    assert.equal((await t.resume({ id: 'm1' })).status, 200);
    await until(() => t.toasts().length, 5000, 'the first toast');
    t.state.broken = true;
    await until(() => t.toasts().length > 1, 5000, 'the second toast');
    assert.deepEqual(t.toasts(), ['Resuming “Film”', TODAY]);
  }));

  test('leaving the app before it\'s ready: nothing is sent, and nothing is said', () => withService({ HOMEPLUS_PREVIEW_LAUNCH_FRONT: '1' }, async (t) => {
    t.state.connected = false;
    assert.equal((await t.resume({ id: 'm1' })).status, 200);
    await t.launch({ id: 'youtube' }); /* (it came to the front, and now another did) */
    t.state.connected = true;
    await sleep(2500); /* a poll has seen the session, and the first toast has shown */
    assert.equal(t.sent().length, 0);
    assert.deepEqual(t.toasts(), ['Resuming “Film”']);
  }));

  test('Home+ still in front right after the launch isn\'t leaving', () => withService({}, async (t) => {
    t.state.connected = false;
    assert.equal((await t.resume({ id: 'm1' })).status, 200);
    t.state.connected = true;
    await until(() => t.sent().length, 5000, 'PlayNow');
  }));

  test('OK on another title: the new one replaces it', () => withService({}, async (t) => {
    t.state.connected = false;
    assert.equal((await t.resume({ id: 'm1' })).status, 200);
    assert.equal((await t.resume({ id: 'e2' })).status, 200);
    t.state.connected = true;
    await until(() => t.sent().length, 5000, 'PlayNow');
    await sleep(1200); /* the first run's next poll, if it had one */
    assert.deepEqual(t.sent().map((h) => h.query.get('itemIds')), ['e2']);
    await until(() => t.toasts().length, 5000, 'the toast');
    assert.deepEqual(t.toasts(), ['Playing “Show”'], 'the new title\'s only');
  }));

  test('a launch that fails is answered like /api/launch\'s, and nothing runs', () => withService({}, async (t) => {
    const r = await t.resume({ app: 'broken', id: 'm1' });
    assert.equal(r.status, 200);
    assert.equal(r.json.returnValue, false);
    assert.deepEqual(t.launches(), ['broken']);
    await sleep(200);
    assert.equal(t.asked('/Sessions/Capabilities').length, 0);
    assert.deepEqual(t.toasts(), []);
  }));

  test('other cards: /api/launch still shows its toast over the app', () => withService({}, async (t) => {
    assert.equal((await t.launch({ id: 'stremio', toast: 'Where it is' })).json.returnValue, true);
    await until(() => t.toasts().length, 5000, 'the toast');
    assert.deepEqual(t.toasts(), ['Where it is']);
  }));
});
