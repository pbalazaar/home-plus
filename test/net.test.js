'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { requestJSON } = require('../service/net');
const { mockServer } = require('./helpers');

test('net: JSON replies are parsed, and the status is passed on', async (t) => {
  const s = await mockServer(() => ({ status: 201, json: { a: 1 } }));
  t.after(s.close);
  const r = await requestJSON('GET', `${s.base}/x`);
  assert.equal(r.status, 201);
  assert.deepEqual(r.json, { a: 1 });
});

test('net: a body is sent as JSON, with its length', async (t) => {
  const s = await mockServer((req, url, body) => ({ json: { got: JSON.parse(body), type: req.headers['content-type'], length: req.headers['content-length'] } }));
  t.after(s.close);
  const r = await requestJSON('POST', `${s.base}/x`, { body: { hi: 'there' } });
  assert.deepEqual(r.json.got, { hi: 'there' });
  assert.equal(r.json.type, 'application/json');
  assert.equal(r.json.length, String(Buffer.byteLength('{"hi":"there"}')));
});

test('net: a reply that isn\'t JSON gives json null', async (t) => {
  const s = await mockServer(() => ({ body: '<html>', type: 'text/html' }));
  t.after(s.close);
  assert.equal((await requestJSON('GET', `${s.base}/x`)).json, null);
});

test('net: HTTP errors resolve; network errors reject', async (t) => {
  const s = await mockServer(() => ({ status: 500, json: {} }));
  const base = s.base;
  assert.equal((await requestJSON('GET', `${base}/x`)).status, 500);
  await s.close();
  await assert.rejects(requestJSON('GET', `${base}/x`)); /* nothing listens there now */
});

test('net: a server that never answers times out', async (t) => {
  const s = await mockServer(() => ({ hang: true }));
  t.after(s.close);
  await assert.rejects(requestJSON('GET', `${s.base}/x`, { timeout: 200 }), /timed out/);
});

test('net: only http and https URLs', async () => {
  await assert.rejects(requestJSON('GET', 'ftp://example.com/x'), /unsupported URL/);
  await assert.rejects(requestJSON('GET', 'not a url'), /bad URL/);
});

test('callers: the luna methods answer the TV\'s own tools and Home+, not other apps', () => {
  const { trusted } = require('../service/callers');
  assert.equal(trusted('com.webos.lunasend-14802'), true, 'luna-send: a terminal, LG Input Hook\'s scripts');
  assert.equal(trusted('com.github.pmiraka.homeplus'), true, 'Home+');
  for (const s of ['youtube.leanback.v4', 'com.webos.app.browser', 'com.webos.lunasendpub-12', 'com.webos.lunasend-', 'com.webos.lunasend-1 x', '', undefined])
    assert.equal(trusted(s), false, `not ${s}`);
});
