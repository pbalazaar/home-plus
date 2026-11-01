/* Titles published by installed streaming apps for LG Home. Their content IDs
 * belong to those apps, unlike TMDB IDs, and can open a title directly. */
'use strict';

const crypto = require('crypto');
const createProviderDetails = require('./provider-details');

const APPMGR = 'luna://com.webos.applicationManager';
const CONTENTS = 'luna://com.webos.service.applicationmanager/getAllContents';
const MAX_ITEMS = 10;
const ROWS_MS = 60 * 1000;
const METHOD_WAIT_MS = 2500;
const DETAIL_WAIT_MS = 3000;

function within(promise, timeout, fallback) {
  let timer;
  return Promise.race([promise, new Promise((resolve) => { timer = setTimeout(() => resolve(fallback), timeout); })])
    .then((value) => { clearTimeout(timer); return value; }, (error) => { clearTimeout(timer); throw error; });
}

module.exports = function createRecommendations({ luna, tmdb, log, region, providerDetails }) {
  const supported = new Map();
  let cachedRows = { at: 0, all: [] };
  let rowsBuilding = null;
  const nativeDetails = providerDetails || createProviderDetails({ tmdb, log, region });

  async function rows(fresh = false) {
    if (!fresh && cachedRows.at && Date.now() - cachedRows.at < ROWS_MS) return cachedRows.all;
    if (!rowsBuilding) {
      rowsBuilding = (async () => {
        const r = await within(luna(CONTENTS, {}), METHOD_WAIT_MS, null);
        const available = r && r.returnValue !== false && Array.isArray(r.allContents);
        const all = available ? r.allContents : [];
        if (!available) log('recommendations: app recommendations unavailable');
        cachedRows = { at: Date.now(), all };
        return all;
      })();
    }
    try { return await rowsBuilding; } finally { rowsBuilding = null; }
  }

  async function canOpen(appId) {
    const old = supported.get(appId);
    if (old && Date.now() - old.at < (old.value ? 3600 * 1000 : ROWS_MS)) return old.value;
    const r = await within(luna(`${APPMGR}/getAppInfo`, { id: appId }), METHOD_WAIT_MS, null);
    let value = false;
    try { value = JSON.parse(((r || {}).appInfo || {}).deeplinkingParams).contentTarget === '$CONTENTID'; } catch (e) { /* no title link */ }
    supported.set(appId, { at: Date.now(), value });
    return value;
  }

  function cards(all, appId) {
    const row = all.find((r) => r.launchPointId === `${appId}_default`);
    if (!row || !Array.isArray(row.contents)) return [];
    return row.contents.filter((c) => c && c.type === 'content' && typeof c.titleText === 'string' && c.titleText.trim() &&
      c.onClick && c.onClick.targetAppId === appId && typeof c.onClick.contentId === 'string' && c.onClick.contentId);
  }

  function itemId(appId, title) {
    return `rec:${crypto.createHash('sha1').update(`${appId}\n${title}`).digest('hex')}`;
  }

  async function featured(appId, appTitle, detailsEnabled = true) {
    const all = await rows();
    const list = cards(all, appId);
    if (!list.length || !(await canOpen(appId))) return null;
    const links = new Map();
    const items = list.filter((c) => {
      const id = itemId(appId, c.titleText);
      if (links.has(id)) { links.get(id).add(c.onClick.contentId); return false; }
      links.set(id, new Set([c.onClick.contentId]));
      return true;
    }).slice(0, MAX_ITEMS);
    return { kind: 'recommendations', provider: '', items: await Promise.all(items.map(async (c) => {
      const id = itemId(appId, c.titleText);
      const info = detailsEnabled ? await within((async () =>
        (await tmdb.detailsByTitle(c.titleText, appTitle)) || nativeDetails.details(appId, c, appTitle)
      )(), DETAIL_WAIT_MS, null) : null;
      const picture = (c.image && c.image.url) || (c.image_4k && c.image_4k.url) || '';
      return Object.assign({
        // Namesakes open the app: a refreshed link cannot identify which title was shown.
        id: links.get(id).size === 1 ? id : id.replace('rec:', 'app:'),
        name: c.titleText, type: '', genre: '', year: 0, rating: 0, overview: '',
        background: picture, logo: ''
      }, info || {}, { background: (info && info.background) || picture });
    })) };
  }

  async function launch(appId, id) {
    if (typeof id !== 'string' || !/^rec:[0-9a-f]{40}$/.exec(id) || !(await canOpen(appId))) return null;
    const matches = cards(await rows(true), appId).filter((c) => itemId(appId, c.titleText) === id);
    const card = matches[0];
    if (!card || matches.some((c) => c.onClick.contentId !== card.onClick.contentId)) return null;
    return luna(`${APPMGR}/launch`, { id: appId,
      replaceParams: { deeplinkingParams: { target: '$CONTENTID', value: card.onClick.contentId } } });
  }

  return { featured, launch };
};
