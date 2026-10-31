'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const createRecommendations = require('../service/recommendations');

test('provider recommendations keep title IDs private and launch the selected title', async () => {
  const calls = [];
  const lookups = [];
  const rows = [{ launchPointId: 'stan.webos2_default', contents: [
    { type: 'content', titleText: 'Crystal Lake', image: { url: 'https://example.com/card.jpg' },
      onClick: { targetAppId: 'stan.webos2', contentId: '186547' } },
    { type: 'content', titleText: 'Wrong app', onClick: { targetAppId: 'other.app', contentId: '1' } },
    { type: 'content', titleText: 'Crystal Lake', onClick: { targetAppId: 'stan.webos2', contentId: '186547' } }
  ] }];
  const luna = async (uri, params) => {
    calls.push({ uri, params });
    if (uri.endsWith('/getAllContents')) return { returnValue: true, allContents: rows };
    if (uri.endsWith('/getAppInfo')) return { returnValue: true, appInfo: { deeplinkingParams: '{"contentTarget":"$CONTENTID"}' } };
    if (uri.endsWith('/launch')) return { returnValue: true };
    throw new Error(uri);
  };
  const recommendations = createRecommendations({ luna, tmdb: { detailsByTitle: async (title, provider) => {
    lookups.push([title, provider]);
    return { type: 'series', genre: 'Horror', year: 2026, rating: 7.4, overview: 'The story.', background: '', logo: '' };
  } }, log: () => {} });
  const shelf = await recommendations.featured('stan.webos2', 'Stan');
  assert.equal(shelf.kind, 'recommendations');
  assert.equal(shelf.items.length, 1);
  assert.equal(shelf.items[0].name, 'Crystal Lake');
  assert.deepEqual(lookups, [['Crystal Lake', 'Stan']]);
  assert.equal(shelf.items[0].genre, 'Horror');
  assert.equal(shelf.items[0].background, 'https://example.com/card.jpg');
  assert.match(shelf.items[0].id, /^rec:[0-9a-f]{40}$/);
  assert.equal(JSON.stringify(shelf).includes('186547'), false, 'provider content IDs stay in the service');
  assert.equal((await recommendations.launch('stan.webos2', shelf.items[0].id)).returnValue, true);
  assert.deepEqual(calls[calls.length - 1], { uri: 'luna://com.webos.applicationManager/launch', params: {
    id: 'stan.webos2', replaceParams: { deeplinkingParams: { target: '$CONTENTID', value: '186547' } }
  } });
  rows[0].contents[0].onClick.contentId = 'fresh-id';
  await recommendations.launch('stan.webos2', shelf.items[0].id);
  assert.equal(calls[calls.length - 1].params.replaceParams.deeplinkingParams.value, 'fresh-id', 'launch rereads expiring app links');
  assert.equal(await recommendations.launch('stan.webos2', 'rec:not-an-id'), null);
});

test('recommendations use only apps with a compatible title-link template', async () => {
  const luna = async (uri) => uri.endsWith('/getAllContents') ? { returnValue: true, allContents: [{
    launchPointId: 'youtube.leanback.v4_default', contents: [{ type: 'content', titleText: 'A video',
      onClick: { targetAppId: 'youtube.leanback.v4', contentId: 'v=abc' } }]
  }] } : { returnValue: true, appInfo: { deeplinkingParams: '{"contentTarget":"v=$CONTENTID"}' } };
  const recommendations = createRecommendations({ luna, tmdb: { detailsByTitle: async () => null }, log: () => {} });
  assert.equal(await recommendations.featured('youtube.leanback.v4'), null);
  assert.equal(await recommendations.launch('youtube.leanback.v4', 'rec:0000000000000000000000000000000000000000'), null);
});

test('unmatched recommendations use provider details without changing their launch ID', async () => {
  const card = { type: 'content', titleText: 'Spike', image: { url: 'https://example.com/spike.jpg' },
    onClick: { targetAppId: 'netflix', contentId: 'private-provider-id' } };
  const calls = [];
  const luna = async (uri, params) => {
    if (uri.endsWith('/getAllContents')) return { returnValue: true, allContents: [{ launchPointId: 'netflix_default', contents: [card] }] };
    if (uri.endsWith('/getAppInfo')) return { appInfo: { deeplinkingParams: '{"contentTarget":"$CONTENTID"}' } };
    if (uri.endsWith('/launch')) { calls.push(params); return { returnValue: true }; }
    throw new Error(uri);
  };
  const providerDetails = { details: async () => ({ type: 'series', genre: 'Kids', year: 2024, rating: 0, overview: 'Friends learn together.' }) };
  const recommendations = createRecommendations({ luna, tmdb: { detailsByTitle: async () => null }, providerDetails, log: () => {} });
  const shelf = await recommendations.featured('netflix', 'Netflix');
  assert.equal(shelf.items[0].overview, 'Friends learn together.');
  assert.equal(shelf.items[0].background, 'https://example.com/spike.jpg');
  assert.equal(JSON.stringify(shelf).includes('private-provider-id'), false);
  await recommendations.launch('netflix', shelf.items[0].id);
  assert.equal(calls[0].replaceParams.deeplinkingParams.value, 'private-provider-id');
});
