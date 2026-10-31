/* Titles published by installed streaming apps for LG Home. Their content IDs
 * belong to those apps, unlike TMDB IDs, and can open a title directly. */
'use strict';

const crypto = require('crypto');
const createProviderDetails = require('./provider-details');

const APPMGR = 'luna://com.webos.applicationManager';
const CONTENTS = 'luna://com.webos.service.applicationmanager/getAllContents';
const MAX_ITEMS = 10;

module.exports = function createRecommendations({ luna, tmdb, log, region, providerDetails }) {
  const supported = new Map();
  const nativeDetails = providerDetails || createProviderDetails({ tmdb, log, region });

  async function rows() {
    const r = await luna(CONTENTS, {});
    if (r && r.returnValue !== false && Array.isArray(r.allContents)) return r.allContents;
    log('recommendations: app recommendations unavailable');
    return [];
  }

  async function canOpen(appId) {
    const old = supported.get(appId);
    if (old && Date.now() - old.at < 3600 * 1000) return old.value;
    const r = await luna(`${APPMGR}/getAppInfo`, { id: appId });
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

  async function featured(appId, appTitle) {
    const all = await rows();
    const list = cards(all, appId);
    if (!list.length || !(await canOpen(appId))) return null;
    const seen = new Set();
    const items = list.filter((c) => {
      const id = itemId(appId, c.titleText);
      if (seen.has(id)) return false;
      seen.add(id);
      return true;
    }).slice(0, MAX_ITEMS);
    return { kind: 'recommendations', provider: '', items: await Promise.all(items.map(async (c) => {
      const info = await tmdb.detailsByTitle(c.titleText, appTitle) || await nativeDetails.details(appId, c, appTitle);
      const picture = (c.image_4k && c.image_4k.url) || (c.image && c.image.url) || '';
      return Object.assign({
        id: itemId(appId, c.titleText), name: c.titleText, type: '', genre: '', year: 0, rating: 0, overview: '',
        background: picture, logo: ''
      }, info || {}, { background: (info && info.background) || picture });
    })) };
  }

  async function launch(appId, id) {
    if (typeof id !== 'string' || !/^rec:[0-9a-f]{40}$/.exec(id) || !(await canOpen(appId))) return null;
    const card = cards(await rows(), appId).find((c) => itemId(appId, c.titleText) === id);
    if (!card) return null;
    return luna(`${APPMGR}/launch`, { id: appId,
      replaceParams: { deeplinkingParams: { target: '$CONTENTID', value: card.onClick.contentId } } });
  }

  return { featured, launch };
};
