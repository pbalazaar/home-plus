/* The image proxy (/img/<key>): pictures from Stremio's addons, Jellyfin and Plex servers and TMDB
 * come through the service, which runs as root on the TV, so it takes only so much of them: up to
 * 8 MB, from a server that keeps sending, and redirected only to another server, not the TV itself.
 * The real service, with the mock Stremio titles' pictures on a server of the test's. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { mockServer, startService, until } = require('./helpers');

const BIG = 9 * 1024 * 1024; /* more than a picture may be */
let svc, dir, art, pics;

test.before(async () => {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-images-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'stremio|Stremio');
  art = await mockServer((req, url) => { /* title i's picture is /i */
    switch (url.pathname) {
      case '/0': return { body: Buffer.alloc(BIG), type: 'image/jpeg' }; /* too big, without saying so */
      case '/1': return { body: Buffer.alloc(BIG), type: 'image/jpeg', headers: { 'Content-Length': BIG } }; /* too big, and says so */
      case '/2': return { hang: true };
      case '/3': return { status: 302, body: '', headers: { Location: `${art.base}/x` } }; /* to the TV itself */
      case '/4': return { body: 'a picture', type: 'image/jpeg' };
      case '/x': return { body: 'one of the TV\'s own services', type: 'text/plain' };
    }
  });
  svc = await startService(dir, { HOMEPLUS_PREVIEW_ART: art.base, HOMEPLUS_FETCH_TIMEOUT_MS: '500' });
  const items = (await svc.call('GET', '/api/continue')).json.items;
  pics = Object.fromEntries(items.map((it) => [it.id.slice(-1), it.background])); /* tt000000<i> */
});

test.after(async () => {
  if (svc) await svc.stop();
  if (art) await art.close();
  fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
});

/* a picture, as the launcher asks for it: its status, how much of it came, and how long it took */
function get(p) {
  return new Promise((resolve) => {
    const t = Date.now();
    let bytes = 0;
    const done = (status) => resolve({ status, bytes, ms: Date.now() - t });
    http.get({ host: '127.0.0.1', port: svc.port, path: p, headers: { 'X-Homeplus-Token': svc.token } }, (res) => {
      res.on('data', (c) => { bytes += c.length; });
      for (const e of ['end', 'aborted', 'error', 'close']) res.on(e, () => done(res.statusCode)); /* (cut short: not 'end') */
    }).on('error', () => done(0));
  });
}
const kept = (p) => path.join(dir, 'config', 'images', p.slice('/img/'.length));

test('images: a picture within the limits comes through, and is kept for next time', async () => {
  const r = await get(pics[4]);
  assert.equal(r.status, 200);
  assert.equal(r.bytes, 'a picture'.length);
  await until(() => fs.existsSync(kept(pics[4])), 5000, 'the picture to be kept');
});

test('images: one over 8 MB isn\'t passed on whole, nor kept', async () => {
  const r = await get(pics[0]); /* its size comes out as it comes: cut short past 8 MB */
  assert.ok(r.status !== 200 || r.bytes < BIG, `${r.status}, ${r.bytes} bytes`);
  assert.equal((await get(pics[1])).status, 502, 'one that says its size up front is refused before it comes');
  const images = path.join(dir, 'config', 'images');
  await until(() => !fs.readdirSync(images).some((f) => f.endsWith('.part')), 5000, 'what came of them to go');
  assert.equal(fs.existsSync(kept(pics[0])), false);
  assert.equal(fs.existsSync(kept(pics[1])), false);
});

test('images: a server that never answers is given up on', async () => {
  const r = await get(pics[2]); /* HOMEPLUS_FETCH_TIMEOUT_MS: 500 ms here (15 s on the TV) */
  assert.equal(r.status, 502);
  assert.ok(r.ms < 2000, `it took ${r.ms} ms`);
});

test('images: a redirect to the TV itself is refused', async () => {
  assert.equal((await get(pics[3])).status, 502);
  assert.equal(art.hits.some((h) => h.path === '/x'), false, 'and not followed');
});
