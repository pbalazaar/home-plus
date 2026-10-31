/* TMDB: what's popular on a streaming service, for the Top Shelf of streaming
 * apps (Netflix, Prime Video, Disney+...), like the Apple TV app's "What to Watch",
 * and what's trending overall, for apps that have no picture of their own.
 *
 * It needs a free TMDB key, set over SSH with tmdbSetup: a v3 API key, or an API
 * Read Access Token (sent as a Bearer token). Without one nothing is fetched.
 * An app is a streaming service when its name is one TMDB lists for the TV's
 * store country. Lists are kept for 12 hours; when TMDB doesn't answer, the old
 * list is served.
 */
'use strict';

const { requestJSON } = require('./net');
const { sortStable } = require('./sort');
const { URLSearchParams } = require('url');

// The preview harness points these at a mock; on the TV they're TMDB's.
const API = process.env.HOMEPLUS_TMDB || 'https://api.themoviedb.org/3';
const IMG = process.env.HOMEPLUS_TMDB_IMAGES || 'https://image.tmdb.org/t/p';
const LIST_MS = 12 * 3600 * 1000;
const PROVIDERS_MS = 24 * 3600 * 1000;
const GENRE_RETRY_MS = 5 * 60 * 1000;
const MIN_ITEMS = 3; // fewer than this, and the app keeps its glow

// "Disney+" is "Disney Plus", "RaiPlay" is "Rai Play": compared without spaces
function norm(s) {
  return String(s || '').toLowerCase().replace(/\+/g, 'plus').replace(/[^a-z0-9]+/g, '');
}

module.exports = function createTmdb({ readConfig, writeConfig, log, region: tvRegion }) {
  let providers = { at: 0, region: '', list: [] }; // [{ id, name, key, priority }]
  let providersBuilding = null;
  const lists = new Map(); // provider id (or 'trending') -> { at, items }
  const building = new Map(); // provider id -> Promise, so one list is fetched once at a time
  const details = new Map(); // IMDb id and type -> { at, value }
  const detailsBuilding = new Map();
  const titleDetails = new Map(); // provider recommendation title -> { at, value }
  const titleBuilding = new Map();
  let genreLists = { until: 0, tv: new Map(), movie: new Map() };
  let genreBuilding = null;

  function key() {
    return readConfig().tmdbKey || '';
  }

  // a Read Access Token is a JWT (three dot-separated parts); a v3 key goes in the query
  function get(path, params = {}, k = key(), timeout = 10000) {
    const bearer = k.split('.').length === 3;
    const q = new URLSearchParams(Object.assign({}, params, bearer ? {} : { api_key: k })).toString();
    return requestJSON('GET', `${API}${path}${q ? `?${q}` : ''}`, { headers: bearer ? { Authorization: `Bearer ${k}` } : {}, timeout });
  }

  async function region() {
    return String(readConfig().tmdbRegion || (await tvRegion()) || 'US').toUpperCase();
  }

  async function loadProviders() {
    const reg = await region();
    if (providers.list.length && providers.region === reg && Date.now() - providers.at < PROVIDERS_MS) return;
    if (!providersBuilding) {
      providersBuilding = (async () => {
        const seen = new Map();
        for (const kind of ['tv', 'movie']) {
          const r = await get(`/watch/providers/${kind}`, { watch_region: reg });
          if (r.status !== 200 || !r.json) throw new Error(`TMDB didn't list the streaming services (${r.status})`);
          for (const p of r.json.results || []) {
            if (seen.has(p.provider_id)) continue;
            const prio = (p.display_priorities || {})[reg];
            seen.set(p.provider_id, { id: p.provider_id, name: p.provider_name, key: norm(p.provider_name), priority: prio != null ? prio : 999 });
          }
        }
        providers = { at: Date.now(), region: reg, list: [...seen.values()] };
        log('tmdb:', providers.list.length, 'streaming services in', reg);
      })();
    }
    try {
      await providersBuilding;
    } finally {
      providersBuilding = null;
    }
  }

  // The services an app could be, best first: the same name, then names ending in
  // the app's ("Prime Video" -> "Amazon Prime Video", but not "YouTube" -> "YouTube
  // Premium"), most prominent first.
  function candidates(title) {
    const t = norm(title);
    if (t.length < 3) return [];
    const exact = providers.list.filter((p) => p.key === t || p.key === `${t}plus`);
    const within = providers.list.filter((p) => !exact.includes(p) && t.length >= 4 && p.key.endsWith(t));
    const byPriority = (a, b) => a.priority - b.priority;
    return [...sortStable(exact, byPriority), ...sortStable(within, byPriority)].slice(0, 3);
  }

  // the title's logo: English or language-free, the best voted
  async function logoFor(kind, id) {
    try {
      const r = await get(`/${kind}/${id}/images`, { include_image_language: 'en,null' });
      const logos = sortStable(((r.json && r.json.logos) || []).slice(), (a, b) =>
        (b.iso_639_1 === 'en') - (a.iso_639_1 === 'en') || (b.vote_average || 0) - (a.vote_average || 0));
      return logos.length ? `${IMG}/w500${logos[0].file_path}` : '';
    } catch (e) {
      return '';
    }
  }

  async function genres() {
    if (Date.now() < genreLists.until) return genreLists;
    if (!genreBuilding) {
      genreBuilding = Promise.all(['tv', 'movie'].map(async (kind) => {
        const r = await get(`/genre/${kind}/list`, { language: 'en-US' });
        if (r.status !== 200 || !r.json) throw new Error(`TMDB genres failed (${r.status})`);
        return new Map((r.json.genres || []).map((g) => [g.id, g.name]));
      })).then(([tv, movie]) => {
        genreLists = { until: Date.now() + PROVIDERS_MS, tv, movie };
        return genreLists;
      }, (e) => {
        log('tmdb:', e.message);
        genreLists.until = Date.now() + GENRE_RETRY_MS;
        return genreLists;
      });
    }
    const result = await genreBuilding;
    genreBuilding = null;
    return result;
  }

  // the 10 most popular titles on a service (series and films by popularity), or
  // trending overall (p null), with backdrops
  async function build(p) {
    const found = [];
    if (p) {
      // what's current, like "What to Watch": series from the last four years and films from
      // the last two, no news, reality, soap or talk shows (popularity alone favours
      // long-running daily shows); a service with few of those gets a looser list
      const since = (years) => new Date(Date.now() - years * 365 * 864e5).toISOString().slice(0, 10);
      const NOT_SHOWS = '10763,10764,10766,10767';
      const tries = [
        { tv: { 'first_air_date.gte': since(4), without_genres: NOT_SHOWS }, movie: { 'primary_release_date.gte': since(2) } },
        { tv: { without_genres: NOT_SHOWS }, movie: {} }
      ];
      for (const t of tries) {
        found.length = 0;
        for (const kind of ['tv', 'movie']) {
          const r = await get(`/discover/${kind}`, Object.assign({
            watch_region: providers.region, with_watch_providers: String(p.id), with_watch_monetization_types: 'flatrate',
            sort_by: 'popularity.desc', include_adult: 'false', language: 'en-US', page: '1', 'vote_count.gte': '20'
          }, t[kind]));
          if (r.status !== 200 || !r.json) throw new Error(`TMDB discover failed (${r.status})`);
          for (const m of r.json.results || []) if (m.backdrop_path) found.push({ kind, m });
        }
        if (found.length >= 6) break;
      }
      sortStable(found, (a, b) => (b.m.popularity || 0) - (a.m.popularity || 0));
    } else {
      const r = await get('/trending/all/day', { language: 'en-US' });
      if (r.status !== 200 || !r.json) throw new Error(`TMDB trending failed (${r.status})`);
      for (const m of r.json.results || []) if (m.backdrop_path && (m.media_type === 'tv' || m.media_type === 'movie')) found.push({ kind: m.media_type, m });
    }
    const top = found.slice(0, 10);
    const names = await genres();
    const logos = await Promise.all(top.map(({ kind, m }) => logoFor(kind, m.id)));
    return top.map(({ kind, m }, i) => ({
      id: `${kind}:${m.id}`,
      type: kind === 'tv' ? 'series' : 'movie',
      name: m.name || m.title || '',
      genre: (Array.isArray(m.genre_ids) ? m.genre_ids : []).map((id) => names[kind].get(id)).filter(Boolean)[0] || '',
      year: parseInt(String(m.first_air_date || m.release_date || '').slice(0, 4), 10) || 0,
      rating: Number(m.vote_average) > 0 && Number(m.vote_average) <= 10 ? Math.round(Number(m.vote_average) * 10) / 10 : 0,
      overview: String(m.overview || '').replace(/\s+/g, ' ').trim(),
      background: `${IMG}/w1280${m.backdrop_path}`,
      logo: logos[i]
    }));
  }

  async function list(p) {
    const id = p ? p.id : 'trending';
    const c = lists.get(id);
    if (c && Date.now() - c.at < LIST_MS) return c.items;
    if (!building.has(id)) {
      building.set(id, build(p).then((items) => {
        lists.set(id, { at: Date.now(), items });
        building.delete(id);
        return items;
      }, (e) => { building.delete(id); throw e; }));
    }
    try {
      return await building.get(id);
    } catch (e) {
      log('tmdb:', p ? p.name : 'trending', e.message);
      return c ? c.items : []; /* the old list, if there is one */
    }
  }

  /* For an app's Top Shelf: what's popular on the streaming service it is
   * ({ kind: 'popular', provider, items }), or for an app with no picture of its
   * own, what's trending ({ kind: 'trending' }); else no items. */
  async function featured(app, hasArt) {
    const none = { kind: '', provider: '', items: [] };
    if (!key() || !app) return none;
    try {
      await loadProviders();
    } catch (e) {
      log('tmdb:', e.message);
    }
    for (const p of candidates(app.title)) {
      const items = await list(p);
      if (items.length >= MIN_ITEMS) return { kind: 'popular', provider: p.name, items };
    }
    if (hasArt) return none;
    const items = await list(null);
    return items.length >= MIN_ITEMS ? { kind: 'trending', provider: '', items } : none;
  }

  // Stremio and Trakt know the IMDb id of a paused title, but not its synopsis.
  // The find response has the same genre ids, date, rating and overview as discover.
  async function detailsByImdb(id, type) {
    if (!key() || !/^tt\d+$/.test(id) || (type !== 'movie' && type !== 'series')) return null;
    const cacheId = `${type}:${id}`;
    const c = details.get(cacheId);
    if (c && Date.now() - c.at < LIST_MS) return c.value;
    if (!detailsBuilding.has(cacheId)) {
      detailsBuilding.set(cacheId, (async () => {
        const kind = type === 'series' ? 'tv' : 'movie';
        const r = await get(`/find/${id}`, { external_source: 'imdb_id', language: 'en-US' });
        if (r.status !== 200 || !r.json) throw new Error(`TMDB find failed (${r.status})`);
        const m = (r.json[`${kind}_results`] || [])[0];
        if (!m) return null;
        const names = await genres();
        return {
          genre: (Array.isArray(m.genre_ids) ? m.genre_ids : []).map((g) => names[kind].get(g)).filter(Boolean)[0] || '',
          year: parseInt(String(m.first_air_date || m.release_date || '').slice(0, 4), 10) || 0,
          rating: Number(m.vote_average) > 0 && Number(m.vote_average) <= 10 ? Math.round(Number(m.vote_average) * 10) / 10 : 0,
          overview: String(m.overview || '').replace(/\s+/g, ' ').trim()
        };
      })().then((value) => {
        details.set(cacheId, { at: Date.now(), value });
        detailsBuilding.delete(cacheId);
        return value;
      }, (e) => {
        log('tmdb: IMDb details', e.message);
        details.set(cacheId, { at: Date.now() - LIST_MS + GENRE_RETRY_MS, value: c ? c.value : null });
        detailsBuilding.delete(cacheId);
        return c ? c.value : null;
      }));
    }
    return detailsBuilding.get(cacheId);
  }

  // A provider card can add a season, release year, regional label or franchise
  // name. These variants only nominate candidates; provider availability must
  // confirm an inexact name before its details are shown.
  function titleVariants(title) {
    const variants = [title];
    const afterColon = title.replace(/^[^:]{2,25}:\s*/, '');
    const withoutRegion = title.replace(/\s+(?:USA|US)$/i, '');
    if (afterColon !== title) variants.push(afterColon);
    if (withoutRegion !== title) variants.push(withoutRegion);
    return variants.map(norm).filter(Boolean);
  }

  function namesFor(m) {
    return [m.name || m.title, m.original_name || m.original_title].map(norm).filter(Boolean);
  }

  function plausibleAlias(m, variants) {
    return namesFor(m).some((name) => variants.some((v) => v === name ||
      (v.length >= 12 && name.length >= 12 &&
        ((v.includes(name) && name.length / v.length >= 0.65) ||
         (name.endsWith(v) && v.length / name.length >= 0.65)))));
  }

  async function providerMatch(matches, providerTitle, reg) {
    if (!providerTitle || !matches.length || matches.length > 8) return null;
    try {
      await loadProviders();
      const provider = candidates(providerTitle)[0];
      if (!provider || providers.region !== reg) return null;
      const available = await Promise.all(matches.map(async (m) => {
        const r = await get(`/${m.media_type}/${m.id}/watch/providers`, {}, key(), 3000);
        if (r.status !== 200 || !r.json) throw new Error(`TMDB availability failed (${r.status})`);
        const offers = (r.json.results || {})[reg] || {};
        return ['flatrate', 'ads', 'free', 'rent', 'buy'].some((kind) =>
          Array.isArray(offers[kind]) && offers[kind].some((p) => p.provider_id === provider.id));
      }));
      const offered = matches.filter((m, i) => available[i]);
      return offered.length === 1 ? offered[0] : null;
    } catch (e) {
      log('tmdb: recommendation availability', e.message);
      return null;
    }
  }

  // Apps publish their own recommendations with title links, but no synopsis or
  // rating. Keep exact or dominant matches; check the TV's provider availability
  // when namesakes or a provider's wording make the title uncertain.
  async function detailsByTitle(input, providerTitle = '') {
    let title = String(input || '').trim();
    const dated = /\s+\(((?:19|20)\d{2})\)$/.exec(title);
    if (dated) title = title.slice(0, -dated[0].length).trim();
    title = title.replace(/\s+-\s+Season\s+\d+$/i, '');
    const titleKey = norm(title);
    if (!key() || titleKey.length < 2) return null;
    const reg = providerTitle ? await region() : '';
    const cacheId = `${norm(providerTitle)}:${reg}:${titleKey}:${dated ? dated[1] : ''}`;
    const c = titleDetails.get(cacheId);
    if (c && Date.now() - c.at < LIST_MS) return c.value;
    if (!titleBuilding.has(cacheId)) {
      titleBuilding.set(cacheId, (async () => {
        const r = await get('/search/multi', { query: title, include_adult: 'false', language: 'en-US', page: 1 });
        if (r.status !== 200 || !r.json) throw new Error(`TMDB search failed (${r.status})`);
        const choices = (r.json.results || []).filter((m) => (m.media_type === 'tv' || m.media_type === 'movie') &&
          (!dated || parseInt(String(m.first_air_date || m.release_date || '').slice(0, 4), 10) === Number(dated[1])));
        const matches = choices.filter((m) => namesFor(m).includes(titleKey))
          .sort((a, b) => (Number(b.popularity) || 0) - (Number(a.popularity) || 0));
        let m = matches[0];
        if (matches.length > 1 && ((Number(m.popularity) || 0) < 20 ||
          (Number(m.popularity) || 0) < 10 * (Number(matches[1].popularity) || 0))) {
          m = await providerMatch(matches, providerTitle, reg);
        } else if (!matches.length) {
          m = await providerMatch(choices.filter((c) => plausibleAlias(c, titleVariants(title))), providerTitle, reg);
        }
        if (!m) return null;
        const kind = m.media_type, names = await genres();
        return {
          type: kind === 'tv' ? 'series' : 'movie',
          genre: (Array.isArray(m.genre_ids) ? m.genre_ids : []).map((g) => names[kind].get(g)).filter(Boolean)[0] || '',
          year: parseInt(String(m.first_air_date || m.release_date || '').slice(0, 4), 10) || 0,
          rating: Number(m.vote_average) > 0 && Number(m.vote_average) <= 10 ? Math.round(Number(m.vote_average) * 10) / 10 : 0,
          overview: String(m.overview || '').replace(/\s+/g, ' ').trim(),
          background: m.backdrop_path ? `${IMG}/w1280${m.backdrop_path}` : '',
          logo: (await logoFor(kind, m.id)) || ''
        };
      })().then((value) => {
        titleDetails.set(cacheId, { at: Date.now(), value });
        titleBuilding.delete(cacheId);
        return value;
      }, (e) => {
        log('tmdb: title details', e.message);
        titleDetails.set(cacheId, { at: Date.now() - LIST_MS + GENRE_RETRY_MS, value: c ? c.value : null });
        titleBuilding.delete(cacheId);
        return c ? c.value : null;
      }));
    }
    return titleBuilding.get(cacheId);
  }

  function status() {
    return { configured: !!key(), region: readConfig().tmdbRegion || providers.region };
  }

  // the country whose catalogs to show, when it isn't the TV's store country; "" goes back to that
  function setRegion(r) {
    r = String(r || '').trim().toUpperCase();
    if (r && !/^[A-Z]{2}$/.test(r)) throw new Error('the region is a two-letter country code, such as AL or US');
    const cfg = readConfig();
    if (r) cfg.tmdbRegion = r;
    else delete cfg.tmdbRegion;
    writeConfig(cfg);
    lists.clear();
    details.clear();
    titleDetails.clear();
    providers = { at: 0, region: '', list: [] };
    return status();
  }

  // checks the key with TMDB before keeping it; an empty key removes it
  async function setKey(k) {
    k = String(k || '').trim();
    const cfg = readConfig();
    if (k) {
      const r = await get('/configuration', {}, k);
      if (r.status === 401) throw new Error("TMDB didn't accept that key");
      if (r.status !== 200) throw new Error(`TMDB didn't answer (${r.status})`);
      cfg.tmdbKey = k;
    } else {
      delete cfg.tmdbKey;
    }
    writeConfig(cfg);
    lists.clear();
    details.clear();
    titleDetails.clear();
    providers = { at: 0, region: '', list: [] };
    return status();
  }

  return { featured, detailsByImdb, detailsByTitle, status, setKey, setRegion };
};
