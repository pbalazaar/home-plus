/* Trakt.tv: device sign-in and "Continue Watching" (paused playback).
 *
 * Trakt needs an API app of your own (free): create one at
 * https://trakt.tv/oauth/applications/new with redirect URI
 * urn:ietf:wg:oauth:2.0:oob, then hand its Client ID and Secret to the
 * service (traktSetup). Sign-in is the device flow: the TV shows a code you
 * enter at trakt.tv/activate - no password ever touches the TV.
 */
'use strict';

const https = require('https');

const API = 'https://api.trakt.tv';
const OOB = 'urn:ietf:wg:oauth:2.0:oob';
const META = 'https://images.metahub.space';

function request(method, path, { body, clientId, token } = {}) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : '';
    const headers = {
      'Content-Type': 'application/json',
      'User-Agent': 'HomePlus/1.4 (webOS)',
      'trakt-api-version': '2'
    };
    if (clientId) headers['trakt-api-key'] = clientId;
    if (token) headers.Authorization = `Bearer ${token}`;
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    const req = https.request(API + path, { method, headers, timeout: 15000 }, (res) => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (c) => { buf += c; });
      res.on('end', () => {
        if (res.complete === false) return; /* cut short: Node 12 ends it all the same ('close' rejects) */
        let json = null;
        try { json = buf ? JSON.parse(buf) : null; } catch (e) { /* non-JSON error page */ }
        resolve({ status: res.statusCode, json });
      });
      // The server hung up halfway through the reply: Node 16 and newer never end it, only close it
      res.on('close', () => { if (res.complete === false) reject(new Error('Trakt connection closed before the reply was complete')); });
    });
    req.on('timeout', () => req.destroy(new Error('Trakt request timed out')));
    req.on('error', reject);
    req.end(data || undefined);
  });
}

module.exports = function createTrakt({ readConfig, writeConfig, log }) {
  let pending = null; // { user_code, verification_url, expires_at }
  let pollTimer = null;
  let cache = { at: 0, items: null };

  function creds() {
    const c = readConfig();
    return { cfg: c, clientId: c.traktClientId, secret: c.traktClientSecret };
  }

  function saveTokens(t) {
    const cfg = readConfig();
    cfg.traktAccess = t.access_token;
    cfg.traktRefresh = t.refresh_token;
    cfg.traktExpires = Date.now() + (t.expires_in || 86400) * 1000;
    writeConfig(cfg);
    cache = { at: 0, items: null };
  }

  async function token() {
    const { cfg, clientId, secret } = creds();
    if (!cfg.traktAccess) return null;
    if (Date.now() < (cfg.traktExpires || 0) - 3600 * 1000) return cfg.traktAccess;
    // refresh an hour before expiry
    const r = await request('POST', '/oauth/token', {
      body: { refresh_token: cfg.traktRefresh, client_id: clientId, client_secret: secret, redirect_uri: OOB, grant_type: 'refresh_token' }
    });
    if (r.status === 200 && r.json && r.json.access_token) {
      saveTokens(r.json);
      log('trakt: token refreshed');
      return r.json.access_token;
    }
    log('trakt: refresh failed', r.status);
    return cfg.traktAccess; // try the old one; a 401 below tells the UI to re-link
  }

  function status() {
    const { cfg, clientId } = creds();
    if (pending && Date.now() > pending.expires_at) pending = null;
    return {
      configured: !!clientId,
      connected: !!cfg.traktAccess,
      pending: pending ? { code: pending.user_code, url: pending.verification_url } : null
    };
  }

  function setup(clientId, secret) {
    const cfg = readConfig();
    cfg.traktClientId = String(clientId || '').trim();
    cfg.traktClientSecret = String(secret || '').trim();
    writeConfig(cfg);
    return status();
  }

  async function startDevice() {
    const { clientId, secret } = creds();
    if (!clientId || !secret) throw new Error('Trakt is not set up: run traktSetup with your Client ID and Secret');
    const r = await request('POST', '/oauth/device/code', { body: { client_id: clientId } });
    if (r.status !== 200 || !r.json) throw new Error(`Trakt device code failed (${r.status})`);
    const d = r.json;
    pending = { user_code: d.user_code, verification_url: d.verification_url, expires_at: Date.now() + d.expires_in * 1000 };
    log('trakt: device code issued', d.user_code);
    clearTimeout(pollTimer);
    const poll = async () => {
      if (!pending || Date.now() > pending.expires_at) { pending = null; return; }
      try {
        const t = await request('POST', '/oauth/device/token', { body: { code: d.device_code, client_id: clientId, client_secret: secret } });
        if (t.status === 200 && t.json && t.json.access_token) {
          saveTokens(t.json);
          pending = null;
          log('trakt: connected');
          return;
        }
        if (t.status === 404 || t.status === 409 || t.status === 410 || t.status === 418) {
          log('trakt: device flow ended', t.status);
          pending = null;
          return;
        }
      } catch (e) {
        log('trakt: poll error', e.message);
      }
      pollTimer = setTimeout(poll, (d.interval || 5) * 1000); // 400 = still waiting, 429 = slow down
    };
    pollTimer = setTimeout(poll, (d.interval || 5) * 1000);
    return status();
  }

  function disconnect() {
    const cfg = readConfig();
    delete cfg.traktAccess;
    delete cfg.traktRefresh;
    delete cfg.traktExpires;
    writeConfig(cfg);
    pending = null;
    cache = { at: 0, items: null };
    return status();
  }

  // Paused playback -> the same item shape as Stremio's Continue Watching.
  function toItem(p) {
    const ep = p.type === 'episode';
    const media = ep ? p.show : p.movie;
    if (!media || !media.ids) return null;
    const imdb = media.ids.imdb || '';
    const runtime = (ep ? p.episode && p.episode.runtime : media.runtime) || 0;
    const progress = Math.max(0, Math.min(1, (p.progress || 0) / 100));
    const season = ep && p.episode ? p.episode.season : 0;
    const episode = ep && p.episode ? p.episode.number : 0;
    const videoId = ep ? `${imdb}:${season}:${episode}` : imdb;
    return {
      id: imdb || `trakt-${media.ids.trakt}`,
      source: 'trakt',
      type: ep ? 'series' : 'movie',
      name: media.title,
      background: imdb ? `${META}/background/large/${imdb}/img` : '',
      thumbnail: imdb ? `${META}/background/medium/${imdb}/img` : '',
      logo: imdb ? `${META}/logo/medium/${imdb}/img` : '',
      poster: '',
      season,
      episode,
      progress,
      remainingMin: runtime ? Math.max(1, Math.round(runtime * (1 - progress))) : 0,
      lastWatched: p.paused_at || '',
      // Trakt doesn't know which app played it; Stremio can open anything with an IMDb id.
      deepLink: imdb ? `stremio:///detail/${ep ? 'series' : 'movie'}/${imdb}/${videoId}` : ''
    };
  }

  async function playback() {
    if (cache.items && Date.now() - cache.at < 60 * 1000) return cache.items;
    try {
      return await fetchPlayback();
    } catch (e) { /* an outage doesn't empty the shelf: the last list stays */
      if (!cache.items) throw e;
      log('trakt: refresh failed, keeping the last list:', e.message);
      cache.at = Date.now();
      return cache.items;
    }
  }

  async function fetchPlayback() {
    const { clientId } = creds();
    const t = await token();
    if (!clientId || !t) return [];
    const r = await request('GET', '/sync/playback?extended=full&limit=20', { clientId, token: t });
    if (r.status === 401) {
      log('trakt: token rejected; sign in again');
      disconnect();
      return [];
    }
    if (r.status !== 200 || !Array.isArray(r.json)) throw new Error(`Trakt playback failed (${r.status})`);
    cache = { at: Date.now(), items: r.json.map(toItem).filter(Boolean) };
    return cache.items;
  }

  return { status, setup, startDevice, disconnect, playback };
};
