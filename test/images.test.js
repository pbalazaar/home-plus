/* The image proxy (/img/<key>): pictures from Stremio's addons, Jellyfin and Plex servers and TMDB
 * come through the service, which runs as root on the TV, so it takes only so much of them: up to
 * 8 MB, from a server that keeps sending, and only from another server, not the TV itself (nor
 * redirected to it).
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

/* a picture, as the launcher asks for it (of service s): its status, how much of it came, whether all of
 * it did, and how long it took; given up on after 10 s (status -1) */
function get(p, s = svc) {
  return new Promise((resolve) => {
    const t = Date.now();
    let bytes = 0, req = null;
    const done = (status, whole) => { clearTimeout(limit); resolve({ status, bytes, whole, ms: Date.now() - t }); };
    const limit = setTimeout(() => { done(-1, false); req.destroy(); }, 10000);
    req = http.get({ host: '127.0.0.1', port: s.port, path: p, headers: { 'X-Homeplus-Token': s.token } }, (res) => {
      res.on('data', (c) => { bytes += c.length; });
      for (const e of ['end', 'aborted', 'error', 'close']) res.on(e, () => done(res.statusCode, res.complete)); /* (cut short: not 'end') */
    }).on('error', () => done(0, false));
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

test('images: a server that trickles a picture in is given up on at its deadline, and nothing of it is kept', async () => {
  let hungUp = false;
  const slow = http.createServer((req, res) => { /* a byte every 50 ms: never quiet for long, never done */
    res.writeHead(200, { 'Content-Type': 'image/jpeg' });
    const tick = setInterval(() => res.write('x'), 50);
    res.on('close', () => { clearInterval(tick); hungUp = true; });
  });
  await new Promise((resolve) => slow.listen(0, '127.0.0.1', resolve));
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-images-slow-'));
  fs.writeFileSync(path.join(d, 'apps.txt'), 'stremio|Stremio');
  const s = await startService(d, { HOMEPLUS_PREVIEW_ART: `http://127.0.0.1:${slow.address().port}`,
    HOMEPLUS_FETCH_TIMEOUT_MS: '500', HOMEPLUS_FETCH_DEADLINE_MS: '1000' }); /* (on the TV: 15 s and 30 s) */
  try {
    const pic = (await s.call('GET', '/api/continue')).json.items.find((it) => it.id.endsWith('4')).background;
    const r = await get(pic, s);
    assert.equal(r.whole, false, 'it was passed on as if whole');
    assert.ok(r.ms >= 900 && r.ms < 3000, `cut short after ${r.ms} ms (its deadline: 1 s)`);
    await until(() => hungUp, 3000, 'the picture\'s server to be hung up on');
    const images = path.join(d, 'config', 'images');
    await until(() => !fs.existsSync(images) || !fs.readdirSync(images).length, 3000, 'what came of it to go');
  } finally {
    await s.stop();
    slow.closeAllConnections();
    await new Promise((resolve) => slow.close(resolve));
    fs.rmSync(d, { recursive: true, force: true, maxRetries: 5 });
  }
});

test('images: a redirect to the TV itself is refused', async () => {
  assert.equal((await get(pics[3])).status, 502);
  assert.equal(art.hits.some((h) => h.path === '/x'), false, 'and not followed');
});

/* another service, without the tests' exception for their own server on 127.0.0.1: the pictures'
 * server, written as base, is the TV itself, so its pictures are refused and never asked for */
async function refusedFromTheTV(base) {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-images-tv-'));
  fs.writeFileSync(path.join(d, 'apps.txt'), 'stremio|Stremio');
  const s = await startService(d, { HOMEPLUS_PREVIEW_ART: base, HOMEPLUS_PREVIEW_ART_STRICT: '1' });
  try {
    const pic = (await s.call('GET', '/api/continue')).json.items.find((it) => it.id.endsWith('4')).background;
    const before = art.hits.length;
    const r = await new Promise((resolve) => {
      http.get({ host: '127.0.0.1', port: s.port, path: pic, headers: { 'X-Homeplus-Token': s.token } }, (res) => {
        res.resume();
        res.on('end', () => resolve(res.statusCode));
      }).on('error', () => resolve(0));
    });
    assert.equal(r, 404);
    assert.equal(art.hits.length, before, 'and not asked for');
  } finally {
    await s.stop();
    fs.rmSync(d, { recursive: true, force: true, maxRetries: 5 });
  }
}

test('images: a picture from the TV itself is refused, not fetched', () => refusedFromTheTV(art.base));

/* Linux connects 0.0.0.0, and so [::ffff:0.0.0.0], to the machine itself, where the test's server listens */
test('images: also when its address is IPv4-mapped ([::ffff:0.0.0.0])', () =>
  refusedFromTheTV(`http://[::ffff:0.0.0.0]:${new URL(art.base).port}`));
