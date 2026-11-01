/* Jellyfin: Continue Watching from your own server.
 *
 * Sign-in is Quick Connect: Home+ finds the server on the local network (it
 * answers a UDP broadcast on port 7359), asks it for a code, and you approve
 * that code in any Jellyfin app you're signed in to (Settings -> Quick
 * Connect). No password touches the TV. A server that isn't on this network
 * can be given by URL instead (setServer, or POST /api/jellyfin/server).
 *
 * The row shows what you're partway through, then the next episodes of the
 * shows you're watching. Emby speaks the same API; only its sign-in differs.
 *
 * OK on a title plays it in Jellyfin's own app (playOnTV). That app reads no
 * launch parameters, but it obeys its server, and the server can be asked.
 */
'use strict';

const crypto = require('crypto');
const dgram = require('dgram');
const { requestJSON, onThisTV } = require('./net');
const { URL } = require('url');

const TICKS_PER_MIN = 600000000; // Jellyfin times are 100 ns ticks
const TV_DEVICE = 'LG Smart TV'; // what Jellyfin's web client calls a webOS TV
// On the C3, with the server on the same network, the app's session came 4.5 s after a cold start and
// playback about 2.5 s after PlayNow: these leave room for a slower server
const SESSION_WAIT_MS = 25000, START_WAIT_MS = 15000, POLL_MS = 1000;
const REGISTER_MS = 4000; // how long a server that takes the call but doesn't answer (it's asleep) is given
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

module.exports = function createJellyfin({ readConfig, writeConfig, log, version }) {
  let pending = null; // { code, secret, url, name, expires_at }
  let error = '';
  let pollTimer = null;
  let cache = { at: 0, items: null };

  function account() {
    return readConfig().jellyfin || null; // { url, name, userId, user, token }
  }

  function deviceId() {
    const cfg = readConfig();
    if (!cfg.jellyfinDevice) {
      cfg.jellyfinDevice = crypto.randomBytes(8).toString('hex');
      writeConfig(cfg);
    }
    return cfg.jellyfinDevice;
  }

  // Jellyfin reads Authorization; older servers (and Emby) X-Emby-Authorization.
  function auth(token) {
    let h = `MediaBrowser Client="Home+", Device="LG TV", DeviceId="${deviceId()}", Version="${version}"`;
    if (token) h += `, Token="${token}"`;
    return { Authorization: h, 'X-Emby-Authorization': h };
  }

  // an answer's address: a web server on the network, not this TV (any device on the network can answer)
  function onNetwork(address) {
    let u;
    try { u = new URL(String(address)); } catch (e) { return false; }
    return (u.protocol === 'http:' || u.protocol === 'https:') && !onThisTV(u.hostname);
  }

  // Servers on this network answer "who is JellyfinServer?" on UDP 7359.
  function discover(ms = 2500) {
    return new Promise((resolve) => {
      const found = new Map();
      const sock = dgram.createSocket('udp4');
      let finished = false;
      const done = () => {
        if (finished) return;
        finished = true;
        try { sock.close(); } catch (e) { /* closed */ }
        resolve([...found.values()]);
      };
      sock.on('message', (msg) => {
        try {
          const s = JSON.parse(String(msg));
          if (s.Address && onNetwork(s.Address)) found.set(s.Id || s.Address, { url: String(s.Address).replace(/\/+$/, ''), name: s.Name || 'Jellyfin' });
        } catch (e) { /* not a Jellyfin answer */ }
      });
      sock.on('error', (e) => { log('jellyfin: discovery failed', e.message); done(); });
      sock.bind(() => {
        try {
          sock.setBroadcast(true);
          sock.send(Buffer.from('who is JellyfinServer?'), 7359, '255.255.255.255');
        } catch (e) { log('jellyfin: discovery failed', e.message); }
        setTimeout(done, ms);
      });
    });
  }

  function status() {
    const a = account();
    if (pending && Date.now() > pending.expires_at) pending = null;
    return {
      connected: !!(a && a.token),
      server: a ? a.name : '',
      user: a ? a.user : '',
      pending: pending ? { code: pending.code, server: pending.name } : null,
      error
    };
  }

  async function finish(url, name, secret) {
    const r = await requestJSON('POST', `${url}/Users/AuthenticateWithQuickConnect`, { headers: auth(), body: { Secret: secret } });
    if (r.status !== 200 || !r.json || !r.json.AccessToken) throw new Error(`sign-in failed (${r.status})`);
    const cfg = readConfig();
    cfg.jellyfin = { url, name, userId: r.json.User && r.json.User.Id, user: r.json.User && r.json.User.Name, token: r.json.AccessToken };
    writeConfig(cfg);
    cache = { at: 0, items: null };
    log('jellyfin: connected to', name, 'as', cfg.jellyfin.user);
  }

  // Finds the server, asks it for a Quick Connect code, then waits for approval.
  async function startLink() {
    error = '';
    const cfg = readConfig();
    let server = cfg.jellyfinServer ? { url: cfg.jellyfinServer, name: 'Jellyfin' } : null;
    if (!server) {
      const found = await discover();
      log('jellyfin: found', found.length, 'server(s)');
      server = found[0] || null;
    }
    if (!server) {
      error = 'No Jellyfin server found on this network.';
      return status();
    }
    const r = await requestJSON('POST', `${server.url}/QuickConnect/Initiate`, { headers: auth() });
    if (r.status === 401 || r.status === 403) {
      error = 'Quick Connect is off on this server. Turn it on in Jellyfin: Dashboard → General.';
      return status();
    }
    if (r.status !== 200 || !r.json || !r.json.Secret) {
      error = `${server.name} didn't start Quick Connect (${r.status}).`;
      return status();
    }
    pending = { code: r.json.Code, secret: r.json.Secret, url: server.url, name: server.name, expires_at: Date.now() + 10 * 60 * 1000 };
    log('jellyfin: Quick Connect code issued by', server.name);
    clearTimeout(pollTimer);
    const poll = async () => {
      const p = pending;
      if (!p || Date.now() > p.expires_at) { pending = null; return; }
      try {
        const c = await requestJSON('GET', `${p.url}/QuickConnect/Connect?secret=${encodeURIComponent(p.secret)}`, { headers: auth() });
        if (c.status === 200 && c.json && c.json.Authenticated) {
          await finish(p.url, p.name, p.secret);
          pending = null;
          return;
        }
        if (c.status === 404) { /* the server forgot the request: start over */
          error = 'The code expired. Try again.';
          pending = null;
          return;
        }
      } catch (e) {
        log('jellyfin: poll error', e.message);
      }
      pollTimer = setTimeout(poll, 3000);
    };
    pollTimer = setTimeout(poll, 3000);
    return status();
  }

  // A server Home+ can't find on its own (another network, or a reverse proxy).
  function setServer(url) {
    const cfg = readConfig();
    const u = String(url || '').trim().replace(/\/+$/, '');
    if (u && !/^https?:\/\//.test(u)) throw new Error('the server URL must start with http:// or https://');
    if (u) cfg.jellyfinServer = u;
    else delete cfg.jellyfinServer;
    writeConfig(cfg);
    return status();
  }

  function forget() {
    const cfg = readConfig();
    delete cfg.jellyfin;
    writeConfig(cfg);
    pending = null;
    error = '';
    cache = { at: 0, items: null };
  }

  // Signing out ends Home+'s session on the server too, so the TV leaves its Devices list.
  async function disconnect() {
    const a = account();
    if (a && a.token) {
      try { await requestJSON('POST', `${a.url}/Sessions/Logout`, { headers: auth(a.token), timeout: 4000 }); } catch (e) { /* best effort */ }
    }
    forget();
    return status();
  }

  // A Jellyfin item -> the shape of Stremio's Continue Watching items.
  function toItem(a, it) {
    const ep = it.Type === 'Episode';
    const ud = it.UserData || {};
    const run = it.RunTimeTicks || 0, pos = ud.PlaybackPositionTicks || 0;
    const img = (id, type, w, tag) => `${a.url}/Items/${id}/Images/${type}?maxWidth=${w}&quality=85${tag ? `&tag=${tag}` : ''}&api_key=${a.token}`;
    const tags = it.ImageTags || {};
    let background = '';
    if (it.BackdropImageTags && it.BackdropImageTags.length) background = img(it.Id, 'Backdrop', 1920, it.BackdropImageTags[0]);
    else if (it.ParentBackdropItemId) background = img(it.ParentBackdropItemId, 'Backdrop', 1920, (it.ParentBackdropImageTags || [])[0]);
    // the 16:9 card: an episode's own still, else a landscape thumb, else the backdrop
    let thumbnail = '';
    if (ep && tags.Primary) thumbnail = img(it.Id, 'Primary', 480, tags.Primary);
    else if (tags.Thumb) thumbnail = img(it.Id, 'Thumb', 480, tags.Thumb);
    else if (it.ParentThumbItemId) thumbnail = img(it.ParentThumbItemId, 'Thumb', 480, it.ParentThumbImageTag);
    else if (background) thumbnail = background.replace('maxWidth=1920', 'maxWidth=480');
    // the title's logo: the series' for an episode
    let logo = '';
    if (tags.Logo) logo = img(it.Id, 'Logo', 500, tags.Logo);
    else if (it.ParentLogoItemId) logo = img(it.ParentLogoItemId, 'Logo', 500, it.ParentLogoImageTag);
    return {
      id: it.Id,
      source: 'jellyfin',
      type: ep ? 'series' : 'movie',
      name: ep ? it.SeriesName || it.Name : it.Name,
      background,
      thumbnail,
      logo,
      poster: '',
      season: ep ? it.ParentIndexNumber || 0 : 0,
      episode: ep ? it.IndexNumber || 0 : 0,
      positionTicks: pos,
      progress: run ? Math.max(0, Math.min(1, pos / run)) : (ud.PlayedPercentage || 0) / 100,
      remainingMin: run ? Math.max(1, Math.round((run - pos) / TICKS_PER_MIN)) : 0,
      lastWatched: ud.LastPlayedDate || '',
      deepLink: ''
    };
  }

  async function call(method, a, path, timeout) {
    const r = await requestJSON(method, a.url + path, { headers: auth(a.token), timeout });
    if (r.status === 401) {
      log('jellyfin: token rejected; sign in again');
      forget();
      return null;
    }
    return r;
  }

  const get = (a, path) => call('GET', a, path);

  const QUERY = 'limit=16&mediaTypes=Video&enableImageTypes=Primary,Backdrop,Thumb,Logo&imageTypeLimit=1&enableUserData=true';

  // fresh: read it again, even if the list is new (as an outage does, it keeps the last list)
  async function items(fresh) {
    if (!fresh && cache.items && Date.now() - cache.at < 60 * 1000) return cache.items;
    try {
      return await fetchItems();
    } catch (e) { /* an outage doesn't empty the shelf: the last list stays */
      if (!cache.items) throw e;
      log('jellyfin: refresh failed, keeping the last list:', e.message);
      cache.at = Date.now();
      return cache.items;
    }
  }

  async function fetchItems() {
    const a = account();
    if (!a || !a.token) return [];
    // 10.9 and later; older servers and Emby have the per-user route
    let resume = await get(a, `/UserItems/Resume?userId=${a.userId}&${QUERY}`);
    if (resume && resume.status === 404) resume = await get(a, `/Users/${a.userId}/Items/Resume?${QUERY}`);
    if (!resume) return [];
    if (resume.status !== 200 || !resume.json) throw new Error(`Jellyfin resume failed (${resume.status})`);
    const next = await get(a, `/Shows/NextUp?userId=${a.userId}&limit=10&enableImageTypes=Primary,Backdrop,Thumb,Logo&imageTypeLimit=1&enableUserData=true`);
    const list = (resume.json.Items || []).map((it) => toItem(a, it));
    const seen = new Set(list.map((it) => it.id));
    for (const it of (next && next.status === 200 && next.json && next.json.Items) || []) {
      if (!seen.has(it.Id)) list.push(toItem(a, it));
    }
    cache = { at: Date.now(), items: list.slice(0, 16) };
    return cache.items;
  }

  // A title as the list has it now. OK doesn't wait on the server before its app opens: it may be asleep, and
  // playOnTV reads the list again anyway. Only with no list yet (the service has just started) is it asked.
  async function known(id) {
    return (cache.items || await items()).find((it) => it.id === id);
  }

  async function sessions(a) {
    const r = await get(a, '/Sessions');
    if (!r) throw new Error('signed out');
    if (r.status !== 200 || !Array.isArray(r.json)) throw new Error(`sessions failed (${r.status})`);
    return r.json;
  }

  // This TV's sessions in the server's list: the ones named as Jellyfin's app names a TV, reaching the server
  // from the address Home+ does. Home+'s own entry shows that address, which Docker or a proxy can make
  // something other than the TV's. Client can't say which they are (it depends on when the app signed in), nor
  // can DeviceId (the TV's two sessions don't share one). The address is empty on an entry that hasn't
  // registered, and two empty ones aren't one address.
  function tvSessions(list) {
    const me = list.find((s) => s.DeviceId === deviceId());
    if (!me || !me.RemoteEndPoint) return [];
    return list.filter((s) => s.DeviceName === TV_DEVICE && s.RemoteEndPoint === me.RemoteEndPoint);
  }

  // OK on a title (item: { id, fromStart }): tells Jellyfin's app on this TV to play it, from where it stopped.
  // The app has just been opened: a hidden one doesn't play, it holds the command till it's opened. Answers
  // whether the title started; stillWanted() going false ends it at once, and it stays quiet about that.
  async function playOnTV(item, stillWanted, { sessionMs = SESSION_WAIT_MS, startMs = START_WAIT_MS, pollMs = POLL_MS, registerMs = REGISTER_MS } = {}) {
    const began = Date.now();
    const say = (...what) => log(`jellyfin: resume ${item.id} (${((Date.now() - began) / 1000).toFixed(1)} s):`, ...what);
    try {
      const a = account();
      if (!a) {
        say('signed out');
        return false;
      }
      // First, so a server that isn't there is found out at once. It notes where it sees Home+ from when Home+
      // registers, whatever entry it may have had
      const registered = await call('POST', a, '/Sessions/Capabilities', registerMs);
      if (!registered || registered.status >= 300) throw new Error(`registering failed (${registered ? registered.status : 'signed out'})`);
      const title = (await items(true)).find((it) => it.id === item.id); /* its position, as of now */
      if (!title) {
        say('it has left the list');
        return false;
      }
      const end = Date.now() + sessionMs;
      let session, doubled = false;
      for (;;) {
        if (!stillWanted()) return false;
        const open = tvSessions(await sessions(a)).filter((s) => s.SupportsRemoteControl);
        if (open.length === 1) {
          session = open[0];
          break;
        }
        if (open.length > 1 && !doubled) { /* it never guesses */
          doubled = true;
          say('two sessions at this address: picking neither');
        }
        if (Date.now() >= end) {
          say('no session for this TV');
          return false;
        }
        await sleep(pollMs);
      }
      if (!stillWanted()) return false; /* (it may have left while the list was on its way) */
      const ticks = item.fromStart ? 0 : title.positionTicks;
      say('PlayNow from', ticks, 'to', session.Client, session.Id);
      const sent = await call('POST', a, `/Sessions/${encodeURIComponent(session.Id)}/Playing?playCommand=PlayNow&itemIds=${encodeURIComponent(item.id)}&startPositionTicks=${ticks}`);
      if (!sent || sent.status >= 300) throw new Error(`PlayNow failed (${sent ? sent.status : 'signed out'})`);
      // not only the session it went to: playback is often reported on another of this TV's
      for (const until = Date.now() + startMs; Date.now() < until;) {
        await sleep(pollMs);
        if (!stillWanted()) return false;
        const playing = tvSessions(await sessions(a)).find((s) => s.NowPlayingItem && s.NowPlayingItem.Id === item.id);
        if (playing) {
          say('playing on', playing.Client, playing.Id);
          return true;
        }
      }
      say('it didn\'t start');
      return false;
    } catch (e) { /* the server can't be reached, or won't: the app stays open */
      say('failed:', e.message);
      return false;
    }
  }

  return { status, startLink, setServer, disconnect, items, known, playOnTV };
};
