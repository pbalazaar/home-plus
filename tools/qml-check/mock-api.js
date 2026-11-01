// The service's API as the Control Center over other apps uses it, for tools/qml-check/run.sh:
// POST /api/control answers with the state, a layout as it was sent but slowly (like a busy TV, so
// the check can edit again before the answer comes); POST /api/overlay is recorded, but for asks for
// a frame, answered with frame 1 (the panel's frame file), then none newer, and for Recents; POST
// /api/settings answers with sample pages (a row changes them); POST /api/close says yes. GET /log is
// everything it got, with the token each request carried.
'use strict';
const http = require('http');
const path = require('path');

const got = [];
let controls = null;
const CATALOG = [ /* as check.qml's sample */
  { id: 'power', title: 'Power Off', kind: 'action', icon: 'power', group: 'TV', sizes: ['large', 'small'], available: true, placed: true },
  { id: 'soundMode', title: 'Sound Mode', kind: 'stepper', icon: 'soundMode', group: 'Picture & Sound', sizes: ['wide', 'small'], available: true, placed: false },
  { id: 'brightness', title: 'OLED Brightness', kind: 'stepper', icon: 'brightness', group: 'Picture & Sound', sizes: ['wide', 'small'], available: true, placed: false },
  { id: 'settings', title: 'Settings', kind: 'opens', icon: 'settings', group: 'Home+', sizes: ['small'], available: true, placed: true, pinned: true }
];
// Settings, as the service sends it (settings.js: state()), shorter
const GLYPH = `file://${path.join(__dirname, '../../native/qml/glyphs/settings.png')}`; /* an app's icon, as a file */
const prefs = { dimMinutes: 5, reduceMotion: false, reduceTransparency: false, sounds: false, clock24: false };
let look = 'default';
const step = (id, label, value) => ({ id, kind: 'step', label, value, desc: `What ${label} does.` });
const pages = () => [
  { id: 'display', name: 'Display & OLED', about: 'Looking after the screen while Home+ is on it.', icon: 'brightness',
    rows: [step('dimAfter', 'Dim After', `${prefs.dimMinutes} min`)].concat(['a', 'b', 'c', 'd', 'e', 'f', 'g'].map((n) => step(n, `Row ${n}`, 'On'))) },
  { id: 'tiles', name: 'App Tiles', about: 'How every app’s tile looks.', icon: 'tiles', rows: [
    { id: 'look-default', kind: 'check', label: 'Default', desc: 'Each app’s own colours.', on: look === 'default' },
    { id: 'look-tinted', kind: 'check', label: 'Tinted', desc: 'Glass in a colour.', on: look === 'tinted' }
  ].concat(look === 'tinted' ? [{ id: 'tint', kind: 'tint', label: 'Red', desc: 'Left and Right pick another colour.', colours: ['#ff453a', '#ff9f0a'], on: 0 }] : []) },
  { id: 'accessibility', name: 'Accessibility', about: 'Less motion, and less see-through glass.', icon: 'access', rows: [
    step('reduceMotion', 'Reduce Motion', prefs.reduceMotion ? 'On' : 'Off'), step('reduceTransparency', 'Reduce Transparency', prefs.reduceTransparency ? 'On' : 'Off')
  ] },
  { id: 'accounts', name: 'Accounts', about: 'Continue Watching, and what’s popular on your apps.', icon: 'person', body: 'A line about it.\nAnd another.',
    rows: [{ id: 'trakt', kind: 'home', page: 'accounts', label: 'Trakt', value: 'Connected', desc: 'Paused playback.' }] },
  { id: 'tv', name: 'TV Settings', about: 'LG’s own settings.', icon: 'tv',
    rows: [{ id: 'tv-settings', kind: 'app', app: 'com.palm.app.settings', label: 'All Settings', value: '', desc: '' }] },
  { id: 'about', name: 'About', about: 'Home+ itself, and removing it.', icon: 'info', rows: [
    { id: 'remove', kind: 'home', page: 'about', destructive: true, label: 'Remove Home+', value: '', desc: 'Undoes everything Home+ set up.' }
  ] }
];
function changeSetting(row, dir) {
  if (row === 'dimAfter') prefs.dimMinutes += dir === -1 ? -1 : 1;
  if (row === 'reduceMotion' || row === 'reduceTransparency' || row === 'clock24') prefs[row] = !prefs[row];
  if (row.startsWith('look-')) look = row.slice(5);
}
const RECENTS = { returnValue: true, front: 'youtube', apps: [
  { id: 'youtube', title: 'YouTube', color: '#ff0000', icon: GLYPH }, { id: 'stremio', title: 'Stremio', color: '', icon: GLYPH },
  { id: 'hdmi1', title: 'HDMI 1', color: '#3a3a40', icon: '' }
] };

http.createServer((req, res) => {
  res.setHeader('Content-Type', 'application/json');
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    if (req.method === 'GET' && req.url === '/log') return res.end(JSON.stringify(got));
    let b = {};
    try { b = JSON.parse(body || '{}'); } catch (e) { /* not JSON */ }
    if (req.url === '/api/overlay' && b.event === 'frame') { /* one frame (the file the panel was given), then none newer */
      const after = Number(b.after) || 0;
      return setTimeout(() => res.end(JSON.stringify({ returnValue: true, n: Math.max(after, 1), picture: true })), after ? 2000 : 50);
    }
    got.push({ url: req.url, body: b, token: req.headers['x-homeplus-token'] || '' });
    if (req.url === '/api/overlay' && b.event === 'recents') return res.end(JSON.stringify(RECENTS));
    if (req.url === '/api/settings') {
      if (b.row === 'refused') { res.statusCode = 500; return res.end('{"returnValue":false,"errorText":"refused"}'); }
      if (b.row) changeSetting(b.row, b.dir);
      return res.end(JSON.stringify({ returnValue: true, pages: pages(), prefs }));
    }
    if (req.url !== '/api/control') return res.end('{"returnValue":true}');
    if (b.action === 'layout') controls = b.controls;
    const reply = JSON.stringify({ returnValue: true, catalog: CATALOG, controls: (controls || []).map((c) => (
      { id: c.id, size: c.size, title: c.id, kind: 'action', icon: 'controls', value: '', on: null, available: true })) });
    setTimeout(() => res.end(reply), b.action === 'layout' ? 600 : 50);
  });
}).listen(Number(process.argv[2]) || 47999, '127.0.0.1');
