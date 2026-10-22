/* TMDB's and Trakt's keys, pasted on a phone: the launcher opens the keys page and shows its link as a QR code;
 * the page is on the local network, at a path no one can guess, until the keys are saved. The real service, with a
 * stand-in TMDB that takes any key but "bad". */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { startService } = require('./helpers');
const qr = require('../service/qr');

let svc, dir, tmdbServer;
const call = (...args) => svc.call(...args);

/* the page, as a phone asks for it */
function fetchPage(url, form) {
  return new Promise((resolve, reject) => {
    const body = form ? new URLSearchParams(form).toString() : '';
    const req = http.request(url, { method: form ? 'POST' : 'GET', headers: form ? { 'Content-Type': 'application/x-www-form-urlencoded', 'Content-Length': Buffer.byteLength(body) } : {} }, (res) => {
      let buf = '';
      res.on('data', (c) => { buf += c; });
      res.on('end', () => resolve({ status: res.statusCode, html: buf }));
    });
    req.on('error', reject);
    req.end(body || undefined);
  });
}

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-keys-page-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix');
  tmdbServer = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    res.writeHead(u.searchParams.get('api_key') === 'bad' ? 401 : 200, { 'Content-Type': 'application/json' });
    res.end('{}');
  });
  await new Promise((r) => tmdbServer.listen(0, '127.0.0.1', r));
  svc = await startService(dir, { HOMEPLUS_KEYS_HOST: '127.0.0.1', HOMEPLUS_TMDB: `http://127.0.0.1:${tmdbServer.address().port}` });
});

test.after(async () => {
  if (svc) await svc.stop();
  tmdbServer.close();
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

test('qr: a link becomes a QR code of the right size, as a PNG', () => {
  const link = 'http://192.168.1.23:40000/0123456789abcdef0123456789abcdef';
  const g = qr.encode(link);
  assert.equal(g.length, 33, 'version 4: up to 62 bytes at level M');
  assert.ok(g[0][0] && g[0][6] && !g[1][1] && g[2][2], 'a finder pattern in the corner');
  const png = qr.png(link, 4);
  assert.equal(png.readUInt32BE(0), 0x89504e47);
  assert.equal(png.readUInt32BE(16), (33 + 8) * 4, 'with its 4-module margin');
  assert.throws(() => qr.encode('x'.repeat(300)), /too long/);
});

test('keys: closed until the launcher opens it; then a link with 128 random bits, and its QR code', async () => {
  assert.deepEqual((await call('GET', '/api/keys')).json, { open: false, url: '', tmdb: false, trakt: false });
  assert.equal((await call('GET', '/api/keys/qr')).status, 404);
  assert.equal((await call('POST', '/api/keys/open', { auth: false })).status, 403, 'only the launcher opens it');
  const st = (await call('POST', '/api/keys/open')).json;
  assert.equal(st.open, true);
  assert.match(st.url, /^http:\/\/127\.0\.0\.1:\d+\/[0-9a-f]{32}$/);
  assert.equal((await call('POST', '/api/keys/open')).json.url, st.url, 'asked again, the same page');
  const port = new URL(st.url).port;
  assert.equal((await fetchPage(`http://127.0.0.1:${port}/`)).status, 404, 'not without the path');
  assert.equal((await fetchPage(`http://127.0.0.1:${port}/${'0'.repeat(32)}`)).status, 404);
  const page = await fetchPage(st.url);
  assert.equal(page.status, 200);
  assert.match(page.html, /name="tmdbKey"/);
  assert.match(page.html, /name="traktClientSecret"/);
});

test('keys: a key TMDB turns down, or half of Trakt\'s, isn\'t kept, and the page stays open', async () => {
  const { url } = (await call('GET', '/api/keys')).json;
  let r = await fetchPage(url, { tmdbKey: 'bad' });
  assert.match(r.html, /TMDB didn(&#39;|')t accept that key/);
  r = await fetchPage(url, { traktClientId: 'abc' });
  assert.match(r.html, /needs both/);
  r = await fetchPage(url, {});
  assert.match(r.html, /Nothing to save/);
  assert.deepEqual((await call('GET', '/api/keys')).json, { open: true, url, tmdb: false, trakt: false });
});

test('keys: saved, the launcher hears of it, and the link stops working', async () => {
  const { url } = (await call('GET', '/api/keys')).json;
  const waiting = call('GET', '/api/events');
  const r = await fetchPage(url, { tmdbKey: ' good-key ', traktClientId: 'id<script>', traktClientSecret: 'secret' });
  assert.match(r.html, /TMDB key saved/);
  assert.match(r.html, /Trakt saved/);
  assert.doesNotMatch(r.html, /<script>/);
  assert.deepEqual((await waiting).json, { events: ['keys'] });
  const cfg = JSON.parse(fs.readFileSync(path.join(dir, 'config', 'config.json'), 'utf8'));
  assert.equal(cfg.tmdbKey, 'good-key');
  assert.equal(cfg.traktClientId, 'id<script>');
  await new Promise((res) => setTimeout(res, 800));
  assert.deepEqual((await call('GET', '/api/keys')).json, { open: false, url: '', tmdb: true, trakt: true });
  await assert.rejects(fetchPage(url), 'closed');
});
