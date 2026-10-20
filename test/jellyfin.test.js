'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { mockServer, configStore, quiet, until } = require('./helpers');
const createJellyfin = require('../service/jellyfin');

const MIN = 600000000; /* Jellyfin times are 100 ns ticks: one minute */
const EPISODE = {
  Id: 'e1', Type: 'Episode', Name: 'Pilot', SeriesName: 'Show', ParentIndexNumber: 1, IndexNumber: 2,
  RunTimeTicks: 50 * MIN, UserData: { PlaybackPositionTicks: 20 * MIN, LastPlayedDate: '2026-10-20T10:00:00Z' },
  ImageTags: { Primary: 'p1' }, ParentBackdropItemId: 'show1', ParentBackdropImageTags: ['bk1'],
  ParentLogoItemId: 'show1', ParentLogoImageTag: 'lg1'
};
const MOVIE = {
  Id: 'm1', Type: 'Movie', Name: 'Film', RunTimeTicks: 100 * MIN, UserData: { PlaybackPositionTicks: 25 * MIN },
  ImageTags: { Logo: 'lg2', Thumb: 'th2' }, BackdropImageTags: ['bk2']
};
const NEXT = { Id: 'e2', Type: 'Episode', Name: 'Next', SeriesName: 'Show', ParentIndexNumber: 1, IndexNumber: 3,
  RunTimeTicks: 50 * MIN, UserData: { PlaybackPositionTicks: 0 }, ImageTags: { Primary: 'p2' } };

/* A stand-in Jellyfin server; mode changes how it behaves. */
let server, mode;
test.before(async () => {
  server = await mockServer((req, url) => {
    const signedIn = /Token="tok"/.test(req.headers.authorization || '');
    switch (url.pathname) {
      case '/QuickConnect/Initiate':
        return mode.quickConnectOff ? { status: 401, json: {} } : { json: { Code: '123456', Secret: 's1' } };
      case '/QuickConnect/Connect':
        return { json: { Authenticated: !!mode.approved } };
      case '/Users/AuthenticateWithQuickConnect':
        return { json: { AccessToken: 'tok', User: { Id: 'u1', Name: 'Ann' } } };
      case '/Sessions/Logout':
        return { status: 204, body: '' };
    }
    if (mode.down) return { status: 500, json: {} };
    if (mode.expired || !signedIn) return { status: 401, json: {} };
    if (url.pathname === '/UserItems/Resume') return mode.oldServer ? { status: 404, json: {} } : { json: { Items: [EPISODE, MOVIE] } };
    if (url.pathname === '/Users/u1/Items/Resume') return { json: { Items: [EPISODE, MOVIE] } };
    if (url.pathname === '/Shows/NextUp') return { json: { Items: [EPISODE, NEXT] } };
    return null;
  });
});
test.after(() => server.close());
test.beforeEach(() => { mode = {}; });

const signedIn = () => ({ jellyfin: { url: server.base, name: 'Home', userId: 'u1', user: 'Ann', token: 'tok' }, jellyfinDevice: 'dev1' });
function makeJellyfin(cfg) {
  const store = configStore(cfg);
  return { jf: createJellyfin({ ...store, log: quiet, version: 'test' }), store };
}

test('jellyfin: what you\'re partway through, then the next episodes, without repeats', async () => {
  const { jf } = makeJellyfin(signedIn());
  const items = await jf.items();
  assert.deepEqual(items.map((it) => it.id), ['e1', 'm1', 'e2']);
  const [ep, movie] = items;
  assert.equal(ep.type, 'series');
  assert.equal(ep.name, 'Show');
  assert.equal(ep.season, 1);
  assert.equal(ep.episode, 2);
  assert.equal(ep.progress, 0.4);
  assert.equal(ep.remainingMin, 30);
  assert.equal(ep.background, `${server.base}/Items/show1/Images/Backdrop?maxWidth=1920&quality=85&tag=bk1&api_key=tok`);
  assert.equal(ep.logo, `${server.base}/Items/show1/Images/Logo?maxWidth=500&quality=85&tag=lg1&api_key=tok`, 'the series\' logo');
  assert.equal(movie.type, 'movie');
  assert.equal(movie.progress, 0.25);
  assert.equal(movie.remainingMin, 75);
  assert.equal(movie.logo, `${server.base}/Items/m1/Images/Logo?maxWidth=500&quality=85&tag=lg2&api_key=tok`);
  assert.equal(movie.thumbnail, `${server.base}/Items/m1/Images/Thumb?maxWidth=480&quality=85&tag=th2&api_key=tok`);
});

test('jellyfin: older servers (and Emby) use the per-user resume route', async () => {
  mode.oldServer = true;
  const { jf } = makeJellyfin(signedIn());
  assert.equal((await jf.items()).length, 3);
  assert.ok(server.hits.some((h) => h.path === '/Users/u1/Items/Resume'));
});

test('jellyfin: a rejected token signs out', async () => {
  mode.expired = true;
  const { jf, store } = makeJellyfin(signedIn());
  assert.deepEqual(await jf.items(), []);
  assert.equal(store.get().jellyfin, undefined);
  assert.equal(jf.status().connected, false);
});

test('jellyfin: a failed refresh keeps the last list', async (t) => {
  const { jf } = makeJellyfin(signedIn());
  const first = await jf.items();
  const now = Date.now();
  t.mock.method(Date, 'now', () => now + 61 * 1000); /* past the minute the list is kept */
  mode.down = true;
  assert.deepEqual(await jf.items(), first);
});

test('jellyfin: Quick Connect turned off says where to turn it on', async () => {
  mode.quickConnectOff = true;
  const { jf } = makeJellyfin({ jellyfinServer: server.base });
  const st = await jf.startLink();
  assert.equal(st.pending, null);
  assert.match(st.error, /Dashboard/);
});

test('jellyfin: sign-in with Quick Connect, approved on another device', async () => {
  const { jf, store } = makeJellyfin({ jellyfinServer: server.base });
  const st = await jf.startLink();
  assert.equal(st.pending.code, '123456');
  mode.approved = true;
  await until(() => jf.status().connected, 8000, 'the sign-in');
  assert.deepEqual(store.get().jellyfin, { url: server.base, name: 'Jellyfin', userId: 'u1', user: 'Ann', token: 'tok' });
});

test('jellyfin: signing out ends the session on the server too', async () => {
  const { jf, store } = makeJellyfin(signedIn());
  await jf.disconnect();
  const logout = server.hits.filter((h) => h.path === '/Sessions/Logout').at(-1);
  assert.ok(logout, 'the server was told');
  assert.match(logout.headers.authorization, /Token="tok"/);
  assert.equal(store.get().jellyfin, undefined);
});

test('jellyfin: a server address must be http or https', () => {
  const { jf, store } = makeJellyfin({});
  assert.throws(() => jf.setServer('ftp://host'), /http/);
  jf.setServer('http://host:8096/');
  assert.equal(store.get().jellyfinServer, 'http://host:8096');
  jf.setServer('');
  assert.equal(store.get().jellyfinServer, undefined);
});
