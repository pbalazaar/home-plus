'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { mockServer, configStore, quiet, until, freshRequire } = require('./helpers');

const EPISODE = { ratingKey: '101', type: 'episode', title: 'North Wind', grandparentTitle: 'Glass City', parentIndex: 3, index: 2,
  viewOffset: 15 * 60000, duration: 45 * 60000, thumb: '/library/metadata/101/thumb', grandparentArt: '/library/metadata/100/art',
  lastViewedAt: 1760990000, Image: [{ type: 'clearLogo', url: '/library/metadata/100/clearLogo/1' }] };
const MOVIE = { ratingKey: '202', type: 'movie', title: 'Paper Planes', viewOffset: 30 * 60000, duration: 120 * 60000,
  thumb: '/library/metadata/202/thumb', art: '/library/metadata/202/art' };

/* plex.tv, clients.plex.tv and two Plex servers ("Mine" under /mine, a friend's under /friend), all here */
let server, mode, deadUrl;
test.before(async () => {
  const dead = await mockServer(() => null);
  deadUrl = dead.base;
  await dead.close(); /* nothing answers there: an address that doesn't work from this TV */
  server = await mockServer((req, url) => {
    const p = url.pathname;
    if (p === '/api/v2/pins' && req.method === 'POST') return { status: 201, json: { id: 42, code: 'ABCD', qr: `${server.base}/qr.png`, expiresIn: 900 } };
    if (p === '/api/v2/pins/42') return { json: { authToken: mode.approved ? 'acct' : null } };
    if (p === '/api/v2/resources') {
      return { json: [
        { name: 'Friend', provides: 'server', owned: false, accessToken: 'f', connections: [{ uri: `${server.base}/friend`, local: true }] },
        { name: 'Mine', provides: 'server,player', owned: true, accessToken: 'srv',
          connections: [{ uri: deadUrl, local: true }, { uri: `${server.base}/mine`, local: false }] }] };
    }
    if (p === '/api/v2/user') return { json: { username: 'ann' } };
    if (p === '/mine/identity' || p === '/friend/identity') return { json: { MediaContainer: {} } };
    if (mode.down) return { status: 500, json: {} };
    if (mode.expired || req.headers['x-plex-token'] !== 'srv') return { status: 401, json: {} };
    if (p === '/mine/hubs/continueWatching') return mode.oldServer ? { status: 404, json: {} } : { json: { MediaContainer: { Hub: [{ Metadata: [EPISODE, MOVIE] }] } } };
    if (p === '/mine/library/onDeck') return { json: { MediaContainer: { Metadata: [EPISODE] } } };
    return null;
  });
  process.env.HOMEPLUS_PLEX_TV = server.base;
  process.env.HOMEPLUS_PLEX_CLIENTS = server.base;
});
test.after(() => server.close());
test.beforeEach(() => { mode = {}; });

const signedIn = () => ({ plexClient: 'client1', plex: { token: 'acct', user: 'ann', url: `${server.base}/mine`, server: 'Mine', serverToken: 'srv' } });
function makePlex(cfg) {
  const store = configStore(cfg);
  const createPlex = freshRequire('../service/plex');
  return { plex: createPlex({ ...store, log: quiet, version: 'test' }), store };
}

test('plex: the server\'s Continue Watching, pictures through its transcoder, and the clear logo', async () => {
  const { plex } = makePlex(signedIn());
  const [ep, movie] = await plex.items();
  const base = `${server.base}/mine`;
  assert.equal(ep.type, 'series');
  assert.equal(ep.name, 'Glass City');
  assert.equal(ep.season, 3);
  assert.equal(ep.episode, 2);
  assert.ok(Math.abs(ep.progress - 1 / 3) < 1e-9);
  assert.equal(ep.remainingMin, 30);
  assert.equal(ep.background, `${base}/photo/:/transcode?width=1920&height=1080&minSize=1&upscale=1&url=${encodeURIComponent(EPISODE.grandparentArt)}&X-Plex-Token=srv`);
  assert.equal(ep.logo, `${base}/library/metadata/100/clearLogo/1?X-Plex-Token=srv`);
  assert.equal(ep.lastWatched, new Date(1760990000 * 1000).toISOString());
  assert.equal(movie.type, 'movie');
  assert.equal(movie.remainingMin, 90);
  assert.equal(movie.logo, '');
});

test('plex: older servers use On Deck', async () => {
  mode.oldServer = true;
  const { plex } = makePlex(signedIn());
  assert.deepEqual((await plex.items()).map((it) => it.id), ['101']);
});

test('plex: a rejected token signs out', async () => {
  mode.expired = true;
  const { plex, store } = makePlex(signedIn());
  assert.deepEqual(await plex.items(), []);
  assert.equal(store.get().plex, undefined);
});

test('plex: a failed refresh keeps the last list', async (t) => {
  const { plex } = makePlex(signedIn());
  const first = await plex.items();
  const now = Date.now();
  t.mock.method(Date, 'now', () => now + 61 * 1000);
  mode.down = true;
  assert.deepEqual(await plex.items(), first);
});

test('plex: sign-in with a PIN picks your own server, at the first address that answers', async () => {
  const { plex, store } = makePlex({ plexClient: 'client1' });
  const st = await plex.startLink();
  assert.deepEqual(st.pending, { code: 'ABCD', url: 'plex.tv/link', qr: `${server.base}/qr.png` });
  mode.approved = true;
  await until(() => plex.status().connected, 10000, 'the sign-in');
  assert.deepEqual(store.get().plex, { token: 'acct', user: 'ann', url: `${server.base}/mine`, server: 'Mine', serverToken: 'srv' });
});
