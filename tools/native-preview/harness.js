// Runs the real Home+ service on this machine with a stubbed webOS runtime,
// fake apps and fake Stremio data, so the native front end can be previewed.
// usage: NODE_PATH=<dir with webos-service stub> node harness.js <assetsDir> <serviceDir>
'use strict';
const fs = require('fs');
const path = require('path');
const https = require('https');
const { EventEmitter } = require('events');

const [assets, serviceDir] = process.argv.slice(2);
const APPS = fs.readFileSync(path.join(assets, 'apps.txt'), 'utf8').trim().split(',').map((s) => s.split('|'));
const COLORS = { netflix: '#E50914', prime: '#00A8E1', disney: '#113CCF', hdmi1: '#3A3A40' };

// Stremio: a signed-in config and five shows mid-watch. The config lives in HOMEPLUS_CONFIG_DIR
// when it's set (tests, and run.sh, which need no root), else where the TV keeps it.
const CONFIG_DIR = process.env.HOMEPLUS_CONFIG_DIR || '/var/lib/homeplus';
const CONFIG_FILE = path.join(CONFIG_DIR, 'config.json');
fs.mkdirSync(CONFIG_DIR, { recursive: true });
// HOMEPLUS_PREVIEW_FRESH: a first install (Home button question not answered yet)
fs.writeFileSync(CONFIG_FILE, JSON.stringify(
  process.env.HOMEPLUS_PREVIEW_FRESH ? { stremioAuthKey: 'preview' } : { stremioAuthKey: 'preview', homeAsked: true, homeMode: 'watch' }));
// HOMEPLUS_PREVIEW_PREFS: launcher preferences to start with, as JSON
if (process.env.HOMEPLUS_PREVIEW_PREFS) {
  const cfg = JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf8'));
  cfg.prefs = JSON.parse(process.env.HOMEPLUS_PREVIEW_PREFS);
  fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg));
}
// The service as it runs on the TV: elevated (root), whoever runs the preview.
// HOMEPLUS_PREVIEW_UNROOTED: as Homebrew Channel installs it, before elevation.
process.getuid = () => (process.env.HOMEPLUS_PREVIEW_UNROOTED ? 1000 : 0);
const SHOWS = ['Severed Signals', 'The Long Orbit', 'Northbound', 'Glass City', 'Midnight Kitchen'];
const library = SHOWS.map((name, i) => ({
  _id: `tt000000${i}`, type: i % 2 ? 'movie' : 'series', name, removed: false,
  state: { timeOffset: 600000 * (i + 2), duration: 3000000 + i * 900000, video_id: i % 2 ? '' : `tt000000${i}:${i + 1}:${i + 3}`,
    lastWatched: `2026-10-${20 - i}T10:00:00Z` }
}));

https.request = (url, opts, cb) => {
  const req = new EventEmitter();
  req.end = () => {
    const res = new EventEmitter();
    res.setEncoding = () => {};
    cb(res);
    res.emit('data', JSON.stringify({ result: String(url).endsWith('datastoreGet') ? library : {} }));
    res.emit('end');
  };
  req.destroy = () => {};
  return req;
};
https.get = (url, opts, cb) => {
  const m = /tt000000(\d)/.exec(String(url));
  const res = fs.createReadStream(path.join(assets, `show${m ? m[1] : 0}.jpg`));
  res.statusCode = /\/logo\//.test(url) ? 404 : 200;
  res.headers = { 'content-type': 'image/jpeg' };
  setImmediate(() => cb(res));
  return new EventEmitter();
};

// HOMEPLUS_PREVIEW_MEDIA: mock Jellyfin, Plex and Stremio sign-in servers (Jellyfin and Plex
// first in the dock). "signin": nobody is signed in, and each sign-in is approved on the
// second poll, as if the code was entered on a phone; anything else: already signed in.
if (process.env.HOMEPLUS_PREVIEW_MEDIA) {
  const http = require('http');
  const PORT = 47900, BASE = `http://127.0.0.1:${PORT}`;
  const signin = process.env.HOMEPLUS_PREVIEW_MEDIA === 'signin';
  const polls = { jellyfin: 0, plex: 0, stremio: 0 };
  const json = (res, code, o) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(o)); };
  const image = (res, file, type) => { res.writeHead(200, { 'Content-Type': type }); fs.createReadStream(path.join(assets, file)).pipe(res); };
  const pick = (s) => [...String(s)].reduce((a, c) => a + c.charCodeAt(0), 0) % 5; // a stable picture per item
  const ticks = (min) => min * 600000000;
  const JF = [
    { Id: 'jf1', Type: 'Episode', Name: 'Signal Lost', SeriesName: 'Orbital', ParentIndexNumber: 1, IndexNumber: 4, RunTimeTicks: ticks(52),
      UserData: { PlaybackPositionTicks: ticks(19), LastPlayedDate: '2026-10-20T19:00:00Z' }, ImageTags: { Primary: 'p' }, BackdropImageTags: ['b'] },
    { Id: 'jf2', Type: 'Movie', Name: 'The Quiet Coast', RunTimeTicks: ticks(118), UserData: { PlaybackPositionTicks: ticks(64) },
      ImageTags: { Thumb: 't' }, BackdropImageTags: ['b'] },
    { Id: 'jf3', Type: 'Episode', Name: 'Tides', SeriesName: 'Harbor Lights', ParentIndexNumber: 2, IndexNumber: 1, RunTimeTicks: ticks(44),
      UserData: { PlaybackPositionTicks: 0 }, ImageTags: { Primary: 'p' }, BackdropImageTags: ['b'] }
  ];
  const PX = [
    { ratingKey: '101', type: 'episode', title: 'North Wind', grandparentTitle: 'Glass City', parentIndex: 3, index: 2, viewOffset: 1500000,
      duration: 2700000, thumb: '/library/metadata/101/thumb', art: '/library/metadata/100/art', lastViewedAt: 1760990000 },
    { ratingKey: '202', type: 'movie', title: 'Paper Planes', viewOffset: 3100000, duration: 6400000,
      thumb: '/library/metadata/202/thumb', art: '/library/metadata/202/art', lastViewedAt: 1760900000 }
  ];
  http.createServer((req, res) => {
    const u = new URL(req.url, BASE);
    const p = u.pathname;
    // Jellyfin
    if (p === '/QuickConnect/Initiate') return json(res, 200, { Code: '482913', Secret: 'jf-secret' });
    if (p === '/QuickConnect/Connect') return json(res, 200, { Authenticated: ++polls.jellyfin >= 2 });
    if (p === '/Users/AuthenticateWithQuickConnect') return json(res, 200, { AccessToken: 'jf-token', User: { Id: 'u1', Name: 'Paolo' }, ServerId: 's1' });
    if (p === '/UserItems/Resume') return json(res, 200, { Items: JF.slice(0, 2) });
    if (p === '/Shows/NextUp') return json(res, 200, { Items: JF.slice(2) });
    if (/^\/Items\/[^/]+\/Images\//.test(p)) return image(res, `show${pick(p)}.jpg`, 'image/jpeg');
    // Plex: plex.tv, clients.plex.tv and the server, all here
    if (p === '/api/v2/pins' && req.method === 'POST') return json(res, 201, { id: 7, code: 'QGVR', qr: `${BASE}/qr.png`, expiresIn: 900 });
    if (p === '/api/v2/pins/7') return json(res, 200, { authToken: ++polls.plex >= 2 ? 'plex-token' : null });
    if (p === '/api/v2/resources') {
      return json(res, 200, [{ name: 'Media Server', provides: 'server', owned: true, accessToken: 'plex-srv', connections: [{ uri: BASE, local: true }] }]);
    }
    if (p === '/api/v2/user') return json(res, 200, { username: 'paolo' });
    if (p === '/identity') return json(res, 200, { MediaContainer: {} });
    if (p === '/hubs/continueWatching') return json(res, 200, { MediaContainer: { Hub: [{ Metadata: PX }] } });
    if (p === '/photo/:/transcode') return image(res, `show${pick(u.searchParams.get('url'))}.jpg`, 'image/jpeg');
    // Stremio's TV sign-in
    if (p === '/api/v2/create') {
      return json(res, 200, { result: { success: true, code: 'VTIW', link: 'https://link.stremio.com/VTIW', qrcode: `${BASE}/qr.png` } });
    }
    if (p === '/api/v2/read') return json(res, 200, ++polls.stremio >= 2 ? { result: { authKey: 'preview' } } : { error: { code: 101, message: 'pending' } });
    if (p === '/qr.png') return image(res, 'qr.png', 'image/png');
    json(res, 404, {});
  }).listen(PORT, '127.0.0.1');
  process.env.HOMEPLUS_PLEX_TV = process.env.HOMEPLUS_PLEX_CLIENTS = process.env.HOMEPLUS_STREMIO_LINK = BASE;
  const cfg = JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf8'));
  cfg.jellyfinServer = BASE; // no LAN broadcast in here: the server's address instead
  cfg.layout = { order: ['jellyfin', 'plex', 'stremio'], hidden: [] };
  if (signin) {
    delete cfg.stremioAuthKey;
  } else {
    cfg.jellyfin = { url: BASE, name: 'Living Room', userId: 'u1', user: 'Paolo', token: 'jf-token' };
    cfg.plex = { token: 'plex-token', user: 'paolo', url: BASE, server: 'Media Server', serverToken: 'plex-srv' };
  }
  fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg));
}

// HOMEPLUS_PREVIEW_TMDB: a mock TMDB with three streaming services (Netflix, Prime Video,
// Disney+), their popular series and films, and logos for some titles.
if (process.env.HOMEPLUS_PREVIEW_TMDB) {
  const http = require('http');
  const PORT = 47901, BASE = `http://127.0.0.1:${PORT}`;
  const json = (res, o) => { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(o)); };
  const image = (res, file, type) => { res.writeHead(200, { 'Content-Type': type }); fs.createReadStream(path.join(assets, file)).pipe(res); };
  const PROVIDERS = [{ provider_id: 8, provider_name: 'Netflix' }, { provider_id: 9, provider_name: 'Amazon Prime Video' },
    { provider_id: 337, provider_name: 'Disney Plus' }, { provider_id: 2, provider_name: 'Apple TV' }];
  const NAMES = ['Northern Lights', 'The Long Orbit', 'Paper Moon', 'Severed Signals', 'Glass City', 'Midnight Kitchen', 'Harbor Lights'];
  const titles = (provider, kind) => NAMES.map((name, i) => ({ i, name })).filter(({ i }) => (i + provider) % 2 === (kind === 'tv' ? 0 : 1))
    .map(({ i, name }) => ({ id: provider * 100 + i, [kind === 'tv' ? 'name' : 'title']: name, popularity: 100 - i,
      [kind === 'tv' ? 'first_air_date' : 'release_date']: `${2020 + i}-05-01`, backdrop_path: `/b${(i + provider) % 5}.jpg` }));
  http.createServer((req, res) => {
    const p = new URL(req.url, BASE).pathname;
    if (/^\/tmdb\/watch\/providers\//.test(p)) return json(res, { results: PROVIDERS });
    const d = /^\/tmdb\/discover\/(tv|movie)$/.exec(p);
    if (d) return json(res, { results: titles(Number(new URL(req.url, BASE).searchParams.get('with_watch_providers')), d[1]) });
    if (p === '/tmdb/trending/all/day') {
      return json(res, { results: [...titles(7, 'tv'), ...titles(7, 'movie')].map((m) => Object.assign({ media_type: m.name ? 'tv' : 'movie' }, m)) });
    }
    const im = /^\/tmdb\/(tv|movie)\/(\d+)\/images$/.exec(p);
    if (im) { const i = Number(im[2]) % 100; return json(res, { logos: i < 3 ? [{ file_path: `/l${i}.png`, iso_639_1: 'en', vote_average: 5 }] : [] }); }
    if (p === '/tmdb/configuration') return json(res, { images: {} });
    let m = /^\/img\/w\d+\/b(\d)\.jpg$/.exec(p);
    if (m) return image(res, `show${m[1]}.jpg`, 'image/jpeg');
    m = /^\/img\/w\d+\/l(\d)\.png$/.exec(p);
    if (m) return image(res, `logo${m[1]}.png`, 'image/png');
    res.writeHead(404); res.end();
  }).listen(PORT, '127.0.0.1');
  process.env.HOMEPLUS_TMDB = `${BASE}/tmdb`;
  process.env.HOMEPLUS_TMDB_IMAGES = `${BASE}/img`;
  const cfg = JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf8'));
  cfg.tmdbKey = 'preview-key';
  cfg.tmdbRegion = 'US';
  fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg));
}

// A stand-in TV for the Control Center: inputs (a soundbar with a PS5 behind it), sound output,
// sleep timer and Wi-Fi, as webOS 23's services answer them; settings changes are kept.
const tv = { soundOutput: 'external_arc', sleepTimer: 'off' };
global.__stubLuna = (uri, params) => {
  if (uri.endsWith('/getAllInputStatus')) {
    return { returnValue: true, arcDevice: true, devices: [
      { id: 'HDMI_1', appId: 'com.webos.app.hdmi1', label: 'HDMI 1', hdmiPlugIn: false },
      { id: 'HDMI_2', appId: 'com.webos.app.hdmi2', label: 'Soundbar', spdProductDescription: 'PS5', hdmiPlugIn: true },
      { id: 'HDMI_3', appId: 'com.webos.app.hdmi3', label: 'HDMI 3', hdmiPlugIn: false }] };
  }
  if (uri.endsWith('/getSystemSettings')) return { returnValue: true, settings: Object.fromEntries((params.keys || []).map((k) => [k, tv[k]])) };
  if (uri.endsWith('/setSystemSettings')) { Object.assign(tv, params.settings); return { returnValue: true }; }
  if (uri.endsWith('connectionmanager/getStatus')) return { returnValue: true, wifi: { state: 'connected', ssid: 'Home Wi-Fi' } };
  if (/\/power\/(powerOff|turnOffScreen)$/.test(uri)) return { returnValue: true };
  return null;
};

global.__stubApps = APPS.map(([id, title]) => ({
  id, title, version: '1.0', folderPath: assets, icon: `${id}.png`, iconColor: COLORS[id] || ''
}));
require(path.join(serviceDir, 'service.js'));
