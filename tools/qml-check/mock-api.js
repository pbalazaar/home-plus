// The service's API as the Control Center over other apps uses it, for tools/qml-check/run.sh:
// POST /api/control answers with the state, a layout as it was sent but slowly (like a busy TV, so
// the check can edit again before the answer comes); POST /api/overlay is recorded, but for asks for
// a frame, answered with frame 1 (the panel's frame file), then none newer. GET /log is
// everything it got, with the token each request carried.
'use strict';
const http = require('http');

const got = [];
let controls = null;
const CATALOG = [ /* as check.qml's sample */
  { id: 'power', title: 'Power Off', kind: 'action', icon: 'power', group: 'TV', sizes: ['large', 'small'], available: true, placed: true },
  { id: 'soundMode', title: 'Sound Mode', kind: 'stepper', icon: 'soundMode', group: 'Picture & Sound', sizes: ['wide', 'small'], available: true, placed: false },
  { id: 'brightness', title: 'OLED Brightness', kind: 'stepper', icon: 'brightness', group: 'Picture & Sound', sizes: ['wide', 'small'], available: true, placed: false },
  { id: 'settings', title: 'Settings', kind: 'opens', icon: 'settings', group: 'Home+', sizes: ['small'], available: true, placed: true, pinned: true }
];
http.createServer((req, res) => {
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
    if (req.url !== '/api/control') return res.end('{"returnValue":true}');
    if (b.action === 'layout') controls = b.controls;
    const reply = JSON.stringify({ returnValue: true, catalog: CATALOG, controls: (controls || []).map((c) => (
      { id: c.id, size: c.size, title: c.id, kind: 'action', icon: 'controls', value: '', on: null, available: true })) });
    setTimeout(() => res.end(reply), b.action === 'layout' ? 600 : 50);
  });
}).listen(Number(process.argv[2]) || 47999, '127.0.0.1');
