'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { mockServer, configStore, quiet, last, freshRequire, until } = require('./helpers');

/* A stand-in TMDB: a few streaming services, their popular titles, trending, logos and keys. */
const PROVIDERS = [
  { provider_id: 8, provider_name: 'Netflix', display_priorities: { US: 1 } },
  { provider_id: 1796, provider_name: 'Netflix basic with Ads', display_priorities: { US: 5 } },
  { provider_id: 9, provider_name: 'Amazon Prime Video', display_priorities: { US: 2 } },
  { provider_id: 337, provider_name: 'Disney Plus', display_priorities: { US: 3 } },
  { provider_id: 21, provider_name: 'Stan', display_priorities: { US: 4 } },
  { provider_id: 188, provider_name: 'YouTube Premium', display_priorities: { US: 9 } }
];
const titles = (provider, kind, n = 4) => Array.from({ length: n }, (_, i) => ({
  id: provider * 100 + i + (kind === 'movie' ? 50 : 0),
  [kind === 'tv' ? 'name' : 'title']: `${kind} ${provider}-${i}`,
  [kind === 'tv' ? 'first_air_date' : 'release_date']: `202${i}-05-01`,
  popularity: 100 - i - (kind === 'movie' ? 0.5 : 0),
  vote_average: i === 3 ? 0 : 8.49 - i / 10,
  overview: i === 3 ? '' : `A ${kind} story  about people\nwho discover an unexpected world.`,
  genre_ids: i === 3 ? [] : [kind === 'tv' ? 18 : 35],
  backdrop_path: `/b${provider}-${kind}-${i}.jpg`
}));

let tmdbServer, down = false, genreDown = false, findHang = false, genreHang = false, availabilityDown = false;
test.before(async () => {
  tmdbServer = await mockServer((req, url) => {
    if (down) return { status: 500, json: {} };
    const auth = req.headers.authorization === 'Bearer good.read.token' || url.searchParams.get('api_key') === 'goodkey';
    if (url.pathname === '/3/configuration') return auth ? { json: { images: {} } } : { status: 401, json: {} };
    if (!auth) return { status: 401, json: {} };
    if (url.pathname.startsWith('/3/watch/providers/')) return { json: { results: PROVIDERS } };
    if (url.pathname === '/3/genre/tv/list') return genreHang ? { hang: true } : genreDown ? { status: 500, json: {} } : { json: { genres: [{ id: 18, name: 'Drama' }] } };
    if (url.pathname === '/3/genre/movie/list') return genreHang ? { hang: true } : genreDown ? { status: 500, json: {} } : { json: { genres: [{ id: 35, name: 'Comedy' }] } };
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
    const watch = /^\/3\/(tv|movie)\/(\d+)\/watch\/providers$/.exec(url.pathname);
    if (watch) {
      if (availabilityDown) return { status: 500, json: {} };
      const on = { 1: [8], 2: [8], 123: [9], 213562: [21], 1001: [8], 1002: [9], 1003: [21], 1004: [9],
        1005: [337], 1006: [337], 1007: [337] }[Number(watch[2])] || [];
      return { json: { results: { US: { flatrate: on.map((id) => ({ provider_id: id })) } } } };
    }
    const found = /^\/3\/find\/(tt\d+)$/.exec(url.pathname);
    if (found) {
      if (findHang) return { hang: true };
      const movie = found[1] === 'tt1234567' ? [{ genre_ids: [35], release_date: '2024-02-01', vote_average: 7.56,
        overview: 'A film  about people\nwho discover an unexpected world.' }] : [];
      const tv = found[1] === 'tt7654321' ? [{ genre_ids: [18], first_air_date: '2012-03-01', vote_average: 8.34,
        overview: 'A series  about people\nwho discover an unexpected world.' }] : [];
      return { json: { movie_results: movie, tv_results: tv } };
    }
    if (url.pathname === '/3/search/multi') {
      const query = url.searchParams.get('query');
      if (query === 'Just Add Magic') return { json: { results: [{ id: 123, media_type: 'tv', name: query,
        genre_ids: [18], first_air_date: '2015-01-01', vote_average: 7.86,
        overview: 'Three friends  find a cookbook.', backdrop_path: '/magic.jpg' }] } };
      if (query === 'Crystal Lake') return { json: { results: [
        { id: 213562, media_type: 'tv', name: query, popularity: 141, genre_ids: [18],
          first_air_date: '2026-01-01', vote_average: 7.23, overview: 'A series  story.', backdrop_path: '/lake.jpg' },
        { id: 1267470, media_type: 'movie', title: query, popularity: 1.7 }
      ] } };
      if (query === 'Same Title') return { json: { results: [
        { id: 1, media_type: 'movie', title: query, popularity: 25 },
        { id: 2, media_type: 'tv', name: query, popularity: 20 }
      ] } };
      if (query === 'The Rainmaker') return { json: { results: [
        { id: 1001, media_type: 'tv', name: query, popularity: 25, genre_ids: [18],
          first_air_date: '2025-01-01', vote_average: 8, overview: 'A new series.', backdrop_path: '/new-rain.jpg' },
        { id: 1002, media_type: 'movie', title: query, popularity: 15, genre_ids: [35],
          release_date: '1997-01-01', vote_average: 7, overview: 'An older film.', backdrop_path: '/old-rain.jpg' }
      ] } };
      if (query === 'Survivor USA') return { json: { results: [
        { id: 1003, media_type: 'tv', name: 'Survivor', popularity: 30, genre_ids: [18],
          first_air_date: '2000-01-01', vote_average: 8, overview: 'The competition.', backdrop_path: '/survivor.jpg' }
      ] } };
      if (query === 'SVU: Special Victims Unit') return { json: { results: [
        { id: 1004, media_type: 'tv', name: 'Law & Order: Special Victims Unit', popularity: 40, genre_ids: [18],
          first_air_date: '1999-01-01', vote_average: 8, overview: 'The detectives.', backdrop_path: '/svu.jpg' }
      ] } };
      if (query === "Tim Burton's The Nightmare Before Christmas") return { json: { results: [
        { id: 1005, media_type: 'movie', title: 'The Nightmare Before Christmas', popularity: 40, genre_ids: [35],
          release_date: '1993-01-01', vote_average: 8, overview: 'A festive film.', backdrop_path: '/nightmare.jpg' }
      ] } };
      if (query === 'Star Wars: The Mandalorian and Grogu') return { json: { results: [
        { id: 1006, media_type: 'movie', title: 'The Mandalorian and Grogu', popularity: 40, genre_ids: [35],
          release_date: '2026-01-01', vote_average: 8, overview: 'A space film.', backdrop_path: '/grogu.jpg' },
        { id: 1007, media_type: 'tv', name: 'Star Wars: The Mandalorian and Grogu Official Podcast', popularity: 20,
          first_air_date: '2026-01-01', overview: 'A podcast.' }
      ] } };
      return { json: { results: [] } };
    }
    return null;
  });
  process.env.HOMEPLUS_TMDB = `${tmdbServer.base}/3`;
  process.env.HOMEPLUS_TMDB_IMAGES = `${tmdbServer.base}/img`;
});
test.after(() => tmdbServer.close());
test.beforeEach(() => { down = genreDown = findHang = genreHang = availabilityDown = false; });

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

test('tmdb: an exact or clearly dominant provider title gets details; close namesakes stay plain', async () => {
  const { tmdb } = makeTmdb();
  const info = await tmdb.detailsByTitle('Just Add Magic - Season 1');
  assert.deepEqual(info, { type: 'series', genre: 'Drama', year: 2015, rating: 7.9,
    overview: 'Three friends find a cookbook.', background: `${tmdbServer.base}/img/w1280/magic.jpg`,
    logo: `${tmdbServer.base}/img/w500/en.png` });
  assert.deepEqual(await tmdb.detailsByTitle('Crystal Lake'), { type: 'series', genre: 'Drama', year: 2026, rating: 7.2,
    overview: 'A series story.', background: `${tmdbServer.base}/img/w1280/lake.jpg`,
    logo: `${tmdbServer.base}/img/w500/en.png` });
  assert.equal(await tmdb.detailsByTitle('Same Title'), null);
  const hits = tmdbServer.hits.filter((h) => h.path.startsWith('/3/search/multi')).length;
  await tmdb.detailsByTitle('Just Add Magic - Season 1');
  assert.equal(tmdbServer.hits.filter((h) => h.path.startsWith('/3/search/multi')).length, hits, 'details are cached');
});

test('tmdb: provider availability resolves close namesakes, with separate caches per provider', async () => {
  const { tmdb } = makeTmdb();
  assert.equal((await tmdb.detailsByTitle('The Rainmaker', 'Netflix')).overview, 'A new series.');
  assert.equal((await tmdb.detailsByTitle('The Rainmaker', 'Prime Video')).overview, 'An older film.');
  assert.equal(await tmdb.detailsByTitle('The Rainmaker', 'Stan'), null, 'another provider cannot reuse the Netflix match');
  assert.equal(await tmdb.detailsByTitle('Same Title', 'Netflix'), null, 'two available namesakes remain plain');
  assert.equal((await tmdb.detailsByTitle('The Rainmaker (1997)', 'Prime Video')).overview, 'An older film.',
    'an explicit year identifies the film');
});

test('tmdb: even a dominant exact name must be on the selected provider', async () => {
  const { tmdb } = makeTmdb();
  assert.equal((await tmdb.detailsByTitle('Crystal Lake', 'Stan')).overview, 'A series story.');
  assert.equal(await tmdb.detailsByTitle('Crystal Lake', 'Netflix'), null, 'a namesake is not given Stan metadata on Netflix');
  assert.equal(await tmdb.detailsByTitle('Just Add Magic', 'Netflix'), null);
  assert.equal((await tmdb.detailsByTitle('Just Add Magic', 'Prime Video')).overview, 'Three friends find a cookbook.');
});

test('tmdb: provider availability confirms likely title aliases', async () => {
  const { tmdb } = makeTmdb();
  assert.equal((await tmdb.detailsByTitle('Survivor USA', 'Stan')).overview, 'The competition.');
  assert.equal((await tmdb.detailsByTitle('SVU: Special Victims Unit', 'Prime Video')).overview, 'The detectives.');
  assert.equal((await tmdb.detailsByTitle("Tim Burton's The Nightmare Before Christmas", 'Disney+')).overview,
    'A festive film.');
  assert.equal((await tmdb.detailsByTitle('Star Wars: The Mandalorian and Grogu', 'Disney+')).overview,
    'A space film.', 'an available podcast with a similar name is not chosen');
  assert.equal(await tmdb.detailsByTitle('Survivor USA'), null, 'an inexact title needs a provider check');
});

test('tmdb: unavailable provider data leaves uncertain cards plain', async () => {
  availabilityDown = true;
  const { tmdb } = makeTmdb();
  assert.equal(await tmdb.detailsByTitle('The Rainmaker', 'Netflix'), null);
  assert.equal(await tmdb.detailsByTitle('Just Add Magic', 'Prime Video'), null,
    'unverified exact titles stay plain when availability cannot be checked');
});

test('tmdb: failed availability requests retry after the short backoff', async (t) => {
  const { tmdb } = makeTmdb();
  availabilityDown = true;
  assert.equal(await tmdb.detailsByTitle('The Rainmaker', 'Netflix'), null);
  availabilityDown = false;
  const now = Date.now();
  t.mock.method(Date, 'now', () => now + 6 * 60 * 1000);
  assert.equal((await tmdb.detailsByTitle('The Rainmaker', 'Netflix')).overview, 'A new series.');
});

test('tmdb: an availability outage keeps previously verified details', async (t) => {
  const { tmdb } = makeTmdb();
  const first = await tmdb.detailsByTitle('The Rainmaker', 'Netflix');
  const now = Date.now();
  t.mock.method(Date, 'now', () => now + 13 * 3600 * 1000);
  availabilityDown = true;
  assert.deepEqual(await tmdb.detailsByTitle('The Rainmaker', 'Netflix'), first);
});

test('tmdb: simultaneous ambiguous cards share the provider list lookup', async () => {
  const { tmdb } = makeTmdb();
  const before = tmdbServer.hits.filter((h) => h.path.startsWith('/3/watch/providers/')).length;
  await Promise.all([
    tmdb.detailsByTitle('The Rainmaker', 'Netflix'),
    tmdb.detailsByTitle('Same Title', 'Netflix')
  ]);
  assert.equal(tmdbServer.hits.filter((h) => h.path.startsWith('/3/watch/providers/')).length - before, 2);
});

test('tmdb: "Netflix" is Netflix, not "Netflix basic with Ads"', async () => {
  const { tmdb } = makeTmdb();
  const f = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.equal(f.kind, 'popular');
  assert.equal(f.provider, 'Netflix');
  assert.equal(last(discovered()).query.get('with_watch_providers'), '8');
  assert.equal(last(discovered()).query.get('watch_region'), 'US');
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

test('tmdb: titles have artwork, type, year, rating and synopsis, most popular first', async () => {
  const { tmdb } = makeTmdb();
  const { items } = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.ok(items.length > 0 && items.length <= 10);
  assert.ok(items.every((it) => it.background.startsWith(`${tmdbServer.base}/img/w1280/`)), 'every title has a backdrop');
  assert.ok(items.every((it) => it.logo === `${tmdbServer.base}/img/w500/en.png`), 'the English logo wins');
  assert.equal(items[0].type, 'series');
  assert.equal(items[0].genre, 'Drama');
  assert.equal(items.find((it) => it.type === 'movie').genre, 'Comedy');
  assert.equal(items[0].year, 2020);
  assert.equal(items[0].rating, 8.5);
  assert.equal(items[0].overview, 'A tv story about people who discover an unexpected world.');
  assert.ok(items.some((it) => it.rating === 0 && it.overview === '' && it.genre === ''), 'titles with missing metadata stay usable');
  assert.ok(!items.some((it) => it.name === 'no picture'));
});

test('tmdb: titles still appear if genre names are unavailable', async () => {
  genreDown = true;
  const { tmdb } = makeTmdb();
  const { items } = await tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  assert.ok(items.length > 0);
  assert.equal(items[0].genre, '');
  assert.equal(items[0].rating, 8.5);
});

test('tmdb: paused IMDb titles have genre, year, rating and synopsis', async () => {
  const { tmdb } = makeTmdb();
  const [movie, duplicate] = await Promise.all([
    tmdb.detailsByImdb('tt1234567', 'movie'), tmdb.detailsByImdb('tt1234567', 'movie')
  ]);
  assert.deepEqual(movie, { genre: 'Comedy', year: 2024, rating: 7.6,
    overview: 'A film about people who discover an unexpected world.' });
  assert.deepEqual(duplicate, movie);
  assert.equal(tmdbServer.hits.filter((h) => h.path === '/3/find/tt1234567').length, 1, 'one lookup for simultaneous requests');
  assert.deepEqual(await tmdb.detailsByImdb('tt7654321', 'series'), { genre: 'Drama', year: 2012, rating: 8.3,
    overview: 'A series about people who discover an unexpected world.' });
  assert.equal(await tmdb.detailsByImdb('tt0000000', 'movie'), null, 'unmatched titles remain usable');
  assert.equal(await tmdb.detailsByImdb('kitsu:123', 'series'), null, 'non-IMDb ids are not looked up');
});

test('tmdb: a stalled IMDb and genre lookup cannot hold the Continue Watching shelf', async () => {
  findHang = genreHang = true;
  const { tmdb } = makeTmdb();
  const started = Date.now();
  assert.equal(await tmdb.detailsByImdb('tt1234567', 'movie'), null);
  assert.ok(Date.now() - started < 5000, 'both requests must time out together, not one after the other');
});

test('tmdb: a shared genre request keeps the Continue Watching deadline', async () => {
  genreHang = true;
  const { tmdb } = makeTmdb();
  const before = tmdbServer.hits.length;
  const featured = tmdb.featured({ id: 'netflix', title: 'Netflix' }, true);
  await until(() => tmdbServer.hits.slice(before).some((h) => h.path.startsWith('/3/genre/')), 3000, 'featured genres requested');
  const started = Date.now();
  const details = await tmdb.detailsByImdb('tt1234567', 'movie');
  assert.ok(Date.now() - started < 3200, 'joining a featured request must keep the short deadline');
  assert.equal(details.overview, 'A film about people who discover an unexpected world.');
  assert.equal(details.genre, '');
  await featured;
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
  const hit = last(tmdbServer.hits);
  assert.equal(hit.headers.authorization, 'Bearer good.read.token');
  assert.equal(hit.query.get('api_key'), null);
});

test('tmdb: the region is a two-letter country code', () => {
  const { tmdb, store } = makeTmdb();
  assert.equal(tmdb.setRegion('al').region, 'AL');
  assert.equal(store.get().tmdbRegion, 'AL');
  assert.throws(() => tmdb.setRegion('ALB'), /two-letter/);
  tmdb.setRegion('');
  assert.equal(store.get().tmdbRegion, undefined);
});
