'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { mockServer, configStore, quiet, freshRequire } = require('./helpers');

/* A stand-in TMDB: a few streaming services, their popular titles, trending, logos and keys. */
const PROVIDERS = [
  { provider_id: 8, provider_name: 'Netflix', display_priorities: { US: 1 } },
  { provider_id: 1796, provider_name: 'Netflix basic with Ads', display_priorities: { US: 5 } },
  { provider_id: 9, provider_name: 'Amazon Prime Video', display_priorities: { US: 2 } },
  { provider_id: 337, provider_name: 'Disney Plus', display_priorities: { US: 3 } },
  { provider_id: 188, provider_name: 'YouTube Premium', display_priorities: { US: 9 } }
];
const titles = (provider, kind, n = 4) => Array.from({ length: n }, (_, i) => ({
  id: provider * 100 + i + (kind === 'movie' ? 50 : 0),
  [kind === 'tv' ? 'name' : 'title']: `${kind} ${provider}-${i}`,
  [kind === 'tv' ? 'first_air_date' : 'release_date']: `202${i}-05-01`,
  popularity: 100 - i - (kind === 'movie' ? 0.5 : 0),
  backdrop_path: `/b${provider}-${kind}-${i}.jpg`
}));

let tmdbServer, down = false;
test.before(async () => {
  tmdbServer = await mockServer((req, url) => {
    if (down) return { status: 500, json: {} };
    const auth = req.headers.authorization === 'Bearer good.read.token' || url.searchParams.get('api_key') === 'goodkey';
    if (url.pathname === '/3/configuration') return auth ? { json: { images: {} } } : { status: 401, json: {} };
    if (!auth) return { status: 401, json: {} };
    if (url.pathname.startsWith('/3/watch/providers/')) return { json: { results: PROVIDERS } };
    const d = /^\/3\/discover\/(tv|movie)$/.exec(url.pathname);
    if (d) {
      const list = titles(Number(url.searchParams.get('with_watch_providers')), d[1]);
      list.push({ id: 999, name: 'no picture', popularity: 1000 }); /* no backdrop: left out */
      return { json: { results: list } };
    }
    if (url.pathname === '/3/trending/all/day') {
      return { json: { results: [...titles(7, 'tv', 3).map((m) => ({ ...m, media_type: 'tv' })),
        ...titles(7, 'movie', 3).map((m) => ({ ...m, media_type: 'movie' })),
        { id: 5, name: 'A Person', media_type: 'person', backdrop_path: '/p.jpg' }] } };
    }
    if (/^\/3\/(tv|movie)\/\d+\/images$/.test(url.pathname)) {
      return { json: { logos: [{ file_path: '/fr.png', iso_639_1: 'fr', vote_average: 9 }, { file_path: '/en.png', iso_639_1: 'en', vote_average: 5 }] } };
    }
    return null;
  });
  process.env.HOMEPLUS_TMDB = `${tmdbServer.base}/3`;
  process.env.HOMEPLUS_TMDB_IMAGES = `${tmdbServer.base}/img`;
});
test.after(() => tmdbServer.close());
test.beforeEach(() => { down = false; });

function makeTmdb(cfg = { tmdbKey: 'goodkey' }) {
  const store = configStore(cfg);
  const createTmdb = freshRequire('../service/tmdb');
  return { tmdb: createTmdb({ ...store, log: quiet, region: async () => 'US' }), store };
}
const discovered = () => tmdbServer.hits.filter((h) => h.path.startsWith('/3/discover/'));

test('tmdb: without a key nothing is fetched', async () => {
  const before = tmdbServer.hits.length;
  const { tmdb } = makeTmdb({});
  assert.deepEqual(await tmdb.featured({ id: 'netflix', title: 'Netflix' }, false), { kind: '', provider: '', items: [] });
  assert.equal(tmdbServer.hits.length, before);
  assert.equal(tmdb.status().configured, false);
});

test('tmdb: "Netflix" is Netflix, not "Netflix basic with Ads"', async () => {
  const { tmdb } = makeTmdb();
  const f = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.equal(f.kind, 'popular');
  assert.equal(f.provider, 'Netflix');
  assert.equal(discovered().at(-1).query.get('with_watch_providers'), '8');
  assert.equal(discovered().at(-1).query.get('watch_region'), 'US');
});

test('tmdb: "Prime Video" is Amazon Prime Video, and "Disney+" is Disney Plus', async () => {
  const { tmdb } = makeTmdb();
  assert.equal((await tmdb.featured({ id: 'amazon', title: 'Prime Video' }, true)).provider, 'Amazon Prime Video');
  assert.equal((await tmdb.featured({ id: 'disney', title: 'Disney+' }, true)).provider, 'Disney Plus');
});

test('tmdb: "YouTube" isn\'t YouTube Premium: with no picture of its own it gets what\'s trending', async () => {
  const { tmdb } = makeTmdb();
  const trending = await tmdb.featured({ id: 'youtube', title: 'YouTube' }, false);
  assert.equal(trending.kind, 'trending');
  assert.ok(trending.items.length >= 3);
  assert.ok(trending.items.every((it) => it.type === 'series' || it.type === 'movie'), 'people are left out');
  /* an app with artwork of its own keeps it */
  assert.equal((await tmdb.featured({ id: 'youtube', title: 'YouTube' }, true)).kind, '');
});

test('tmdb: titles have a backdrop, an English logo, a type and a year, most popular first', async () => {
  const { tmdb } = makeTmdb();
  const { items } = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.ok(items.length > 0 && items.length <= 10);
  assert.ok(items.every((it) => it.background.startsWith(`${tmdbServer.base}/img/w1280/`)), 'every title has a backdrop');
  assert.ok(items.every((it) => it.logo === `${tmdbServer.base}/img/w500/en.png`), 'the English logo wins');
  assert.equal(items[0].type, 'series');
  assert.equal(items[0].year, 2020);
  assert.ok(!items.some((it) => it.name === 'no picture'));
});

test('tmdb: a list is fetched once, then kept', async () => {
  const { tmdb } = makeTmdb();
  await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  const n = discovered().length;
  await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.equal(discovered().length, n);
});

test('tmdb: when TMDB stops answering, the last list stays', async (t) => {
  const { tmdb } = makeTmdb();
  const first = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  const now = Date.now();
  t.mock.method(Date, 'now', () => now + 13 * 3600 * 1000); /* past the 12 hours the list is kept */
  down = true;
  const again = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.deepEqual(again.items, first.items);
});

test('tmdb: a key is checked before it\'s kept; "" removes it', async () => {
  const { tmdb, store } = makeTmdb({});
  await assert.rejects(tmdb.setKey('badkey'), /didn't accept/);
  assert.equal(store.get().tmdbKey, undefined);
  assert.equal((await tmdb.setKey(' goodkey ')).configured, true);
  assert.equal(store.get().tmdbKey, 'goodkey');
  assert.equal((await tmdb.setKey('')).configured, false);
  assert.equal(store.get().tmdbKey, undefined);
});

test('tmdb: a Read Access Token is sent as a Bearer token', async () => {
  const { tmdb } = makeTmdb({});
  await tmdb.setKey('good.read.token');
  const last = tmdbServer.hits.at(-1);
  assert.equal(last.headers.authorization, 'Bearer good.read.token');
  assert.equal(last.query.get('api_key'), null);
});

test('tmdb: the region is a two-letter country code', () => {
  const { tmdb, store } = makeTmdb();
  assert.equal(tmdb.setRegion('al').region, 'AL');
  assert.equal(store.get().tmdbRegion, 'AL');
  assert.throws(() => tmdb.setRegion('ALB'), /two-letter/);
  tmdb.setRegion('');
  assert.equal(store.get().tmdbRegion, undefined);
});
