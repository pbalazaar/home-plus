'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const createProviderDetails = require('../service/provider-details');

test('Prime details use the card GTI, verify the page title and cache the synopsis', async () => {
  const id = 'amzn1.dv.gti.12345678-1234-1234-1234-123456789abc';
  const card = { titleText: 'Ben Azelart - Season 1', catalog: { season_number: 1, release_year: 2026 },
    onClick: { contentId: `detail?gti=${id}&refMarker=unused` } };
  const calls = [];
  const details = createProviderDetails({ tmdb: {}, log: () => {}, getPage: async (host, route) => {
    calls.push([host, route]);
    return '<title>Prime Video: Ben Azelart - Season 1</title><meta name="description" content="Ben takes on extreme challenges &amp; builds.">';
  } });
  const first = await details.details('amazon', card, 'Prime Video');
  assert.deepEqual(first, { type: 'series', genre: '', year: 2026, rating: 0,
    overview: 'Ben takes on extreme challenges & builds.' });
  assert.deepEqual(await details.details('amazon', card, 'Prime Video'), first);
  assert.deepEqual(calls, [['www.primevideo.com', `/detail/${id}`]]);
  assert.equal(await details.details('amazon', { titleText: 'Other', onClick: card.onClick }, 'Prime Video'), null);
});

test('Prime live games keep provider descriptions without a TMDB rating', async () => {
  const card = { titleText: 'Knicks vs. 76ers', catalog: { release_year: 2026 },
    onClick: { contentId: 'detail?gti=amzn1.dv.gti.12345678-1234-1234-1234-123456789abc' } };
  const details = createProviderDetails({ tmdb: {}, log: () => {}, getPage: async () =>
    '<title>Watch Knicks vs. 76ers</title><meta content="Watch the Knicks vs. the 76ers live." name="description">' });
  assert.deepEqual(await details.details('amazon', card, 'Prime Video'), { type: '', genre: 'Live sport',
    year: 2026, rating: 0, overview: 'Watch the Knicks vs. the 76ers live.' });
});

test('Netflix details require the card movie ID, matching title and public page URL', async () => {
  const card = { titleText: 'Spike', onClick: { contentId: encodeURIComponent('action=mdp&movieId=83100557&trackId=1') } };
  const page = (name, url) => `<script type="application/ld+json">${JSON.stringify({ '@type': 'TVSeries',
    name, url, startDate: '2024-1-1', genre: 'Kids', description: 'Friends learn together.' })}</script>`;
  const calls = [];
  const details = createProviderDetails({ tmdb: {}, log: () => {}, region: async () => 'AU', getPage: async (host, route) => {
    calls.push([host, route]);
    return page('Spike', 'https://www.netflix.com/au/title/83100557');
  } });
  assert.deepEqual(await details.details('netflix', card, 'Netflix'), { type: 'series', genre: 'Kids',
    year: 2024, rating: 0, overview: 'Friends learn together.' });
  assert.deepEqual(calls, [['www.netflix.com', '/au/title/83100557']]);
  const wrong = createProviderDetails({ tmdb: {}, log: () => {}, region: async () => 'AU',
    getPage: async () => page('Other', 'https://www.netflix.com/au/title/83100557') });
  assert.equal(await wrong.details('netflix', card, 'Netflix'), null);
});

test('Stan page ID and premiere year disambiguate a same-name series and film', async () => {
  const calls = [];
  const tmdb = { detailsByTitle: async (...args) => { calls.push(args); return { type: 'series', year: 2025, overview: 'Rudy starts a new case.' }; } };
  const details = createProviderDetails({ tmdb, log: () => {}, getPage: async (host, route) => {
    assert.deepEqual([host, route], ['www.stan.com.au', '/watch/the-rainmaker/season-1']);
    return '<title>Watch The Rainmaker Season 1 Online | Stan</title><img src="/000/157/286/157286-Cast.jpg">Season 1 • Premiered 2025';
  } });
  const card = { titleText: 'The Rainmaker', onClick: { contentId: '157286' } };
  assert.deepEqual(await details.details('stan.webos2', card, 'Stan.'), { type: 'series', year: 2025, overview: 'Rudy starts a new case.' });
  assert.deepEqual(calls, [['The Rainmaker (2025)', 'Stan.']]);
  const wrong = createProviderDetails({ tmdb, log: () => {}, getPage: async () =>
    '<title>Watch The Rainmaker Season 1 Online | Stan</title><img src="/000/999/999/999999-Cast.jpg">Premiered 2025' });
  assert.equal(await wrong.details('stan.webos2', card, 'Stan.'), null);
});

test('failed provider pages fall back quietly and are not refetched on every shelf request', async () => {
  let calls = 0;
  const details = createProviderDetails({ tmdb: {}, log: () => {}, getPage: async () => { calls++; throw new Error('offline'); } });
  const card = { titleText: 'Ben Azelart', onClick: { contentId: 'detail?gti=amzn1.dv.gti.12345678-1234-1234-1234-123456789abc' } };
  assert.equal(await details.details('amazon', card, 'Prime Video'), null);
  assert.equal(await details.details('amazon', card, 'Prime Video'), null);
  assert.equal(calls, 1);
});

test('titles without comparable characters are left unenriched', async () => {
  let calls = 0;
  const details = createProviderDetails({ tmdb: {}, log: () => {}, getPage: async () => { calls++; return '<title>Different programme</title>'; } });
  const card = { titleText: 'العربية', onClick: { contentId: 'detail?gti=amzn1.dv.gti.12345678-1234-1234-1234-123456789abc' } };
  assert.equal(await details.details('amazon', card, 'Prime Video'), null);
  assert.equal(calls, 0);
});
