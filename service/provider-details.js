/* Public title details for recommendations that have no reliable TMDB match.
 * The app's own content ID still opens the title; these pages supply display
 * details only, and are accepted only when they identify that same title. */
'use strict';

const https = require('https');

const CACHE_MS = 12 * 3600 * 1000;
const RETRY_MS = 5 * 60 * 1000;
const TIMEOUT_MS = 4000;
const MAX_PAGE = 3 * 1024 * 1024;

function norm(s) {
  return String(s || '').toLowerCase().replace(/[^a-z0-9]+/g, '');
}

function clean(s) {
  return String(s || '').replace(/\s+/g, ' ').trim().slice(0, 1000);
}

function decode(s) {
  return String(s || '').replace(/&(#x[0-9a-f]+|#\d+|amp|quot|apos|lt|gt);/gi, (all, entity) => {
    const named = { amp: '&', quot: '"', apos: "'", lt: '<', gt: '>' };
    const n = entity.toLowerCase();
    if (named[n]) return named[n];
    const cp = n.startsWith('#x') ? parseInt(n.slice(2), 16) : parseInt(n.slice(1), 10);
    return cp > 0 && cp <= 0x10ffff ? String.fromCodePoint(cp) : all;
  });
}

function pageTitle(html) {
  const m = /<title\b[^>]*>([\s\S]*?)<\/title>/i.exec(html);
  return m ? clean(decode(m[1])) : '';
}

function description(html) {
  const tag = /<meta\b[^>]*\bname=["']description["'][^>]*>/i.exec(html);
  const m = tag && /\bcontent=(["'])([\s\S]*?)\1/i.exec(tag[0]);
  return m ? clean(decode(m[2])) : '';
}

function year(s) {
  const y = parseInt(String(s || '').slice(0, 4), 10);
  return y >= 1900 && y <= 2100 ? y : 0;
}

function fetchPage(hostname, path) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let timer;
    const finish = (error, html) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (error) reject(error);
      else resolve(html);
    };
    const req = https.get({ hostname, path, headers: {
      Accept: 'text/html', 'User-Agent': 'HomePlus (webOS)', 'Accept-Encoding': 'identity'
    } }, (res) => {
      if (res.statusCode !== 200 || !/^text\/html\b/i.exec(String(res.headers['content-type'] || ''))) {
        res.resume();
        finish(null, '');
        return;
      }
      const chunks = [];
      let bytes = 0;
      res.on('data', (chunk) => {
        bytes += chunk.length;
        if (bytes > MAX_PAGE) {
          req.abort();
          finish(new Error('provider page too large'));
        } else chunks.push(chunk);
      });
      res.on('end', () => finish(null, Buffer.concat(chunks).toString('utf8')));
      res.on('close', () => { if (res.complete === false) finish(new Error('provider page incomplete')); });
      res.on('error', (e) => finish(e));
    });
    req.on('error', (e) => finish(e));
    timer = setTimeout(() => { req.abort(); finish(new Error('provider page timed out')); }, TIMEOUT_MS);
  });
}

function identity(appId, card) {
  const link = String((card.onClick || {}).contentId || '');
  if (appId === 'amazon') {
    const m = /[?&]gti=(amzn1\.dv\.gti\.[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})(?:&|$)/i.exec(link);
    return m && m[1];
  }
  if (appId === 'netflix') {
    let decoded;
    try { decoded = decodeURIComponent(link); } catch (e) { return null; }
    const m = /(?:^|[?&])movieId=(\d{7,9})(?:&|$)/.exec(decoded);
    return m && m[1];
  }
  if (appId === 'stan.webos2') return /^\d{3,9}$/.exec(link) ? link : null;
  return null;
}

module.exports = function createProviderDetails({ tmdb, log, region, getPage = fetchPage }) {
  const cache = new Map();
  const building = new Map();

  async function lookup(appId, card, appTitle, id) {
    if (appId === 'amazon') {
      const html = await getPage('www.primevideo.com', `/detail/${id}`);
      if (!norm(pageTitle(html)).includes(norm(card.titleText))) return null;
      const overview = description(html);
      if (!overview) return null;
      const catalog = card.catalog || {};
      const sport = /\bvs\.?\b/i.exec(card.titleText) && /\blive\b/i.exec(overview);
      return { type: catalog.season_number ? 'series' : '', genre: sport ? 'Live sport' : '',
        year: year(catalog.release_year), rating: 0, overview };
    }
    if (appId === 'netflix') {
      const country = String(region ? await region() : '').toLowerCase();
      if (!/^[a-z]{2}$/.exec(country)) return null;
      const html = await getPage('www.netflix.com', `/${country}/title/${id}`);
      const script = /<script\b[^>]*type=["']application\/ld\+json["'][^>]*>([\s\S]*?)<\/script>/i.exec(html);
      if (!script) return null;
      let item;
      try { item = JSON.parse(script[1]); } catch (e) { return null; }
      if (norm(item.name) !== norm(card.titleText) || !String(item.url || '').includes(`/title/${id}`)) return null;
      const type = item['@type'] === 'TVSeries' ? 'series' : item['@type'] === 'Movie' ? 'movie' : '';
      const overview = clean(item.description);
      if (!type || !overview) return null;
      const genres = Array.isArray(item.genre) ? item.genre : [item.genre];
      return { type, genre: clean(genres[0]), year: year(item.startDate || item.datePublished), rating: 0, overview };
    }
    if (appId === 'stan.webos2') {
      const slug = String(card.titleText).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
      if (!slug) return null;
      const html = await getPage('www.stan.com.au', `/watch/${slug}/season-1`);
      if (!norm(pageTitle(html)).includes(norm(card.titleText)) || !html.includes(`/${id}-`)) return null;
      const m = /\bPremiered\s+((?:19|20)\d{2})\b/i.exec(html);
      if (!m) return null;
      const details = await tmdb.detailsByTitle(`${card.titleText} (${m[1]})`, appTitle);
      return details && details.type === 'series' && details.year === Number(m[1]) ? details : null;
    }
    return null;
  }

  async function details(appId, card, appTitle) {
    if (!norm(card.titleText)) return null;
    const id = identity(appId, card);
    if (!id) return null;
    const key = `${appId}:${id}:${norm(card.titleText)}`;
    const old = cache.get(key);
    if (old && Date.now() - old.at < (old.value ? CACHE_MS : RETRY_MS)) return old.value;
    if (building.has(key)) return building.get(key);
    const work = (async () => {
      let value = null;
      try { value = await lookup(appId, card, appTitle, id); }
      catch (e) { log('provider details:', appId, e.message); }
      cache.set(key, { at: Date.now(), value });
      building.delete(key);
      return value;
    })();
    building.set(key, work);
    return work;
  }

  return { details };
};
