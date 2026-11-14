'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const provenance = require('../tools/update-provenance');
test('unknown exported origin stays unknown; hashes cover payload without generated metadata/token/cache', t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-provenance-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const app = path.join(dir, 'app'), service = path.join(dir, 'service');
  fs.mkdirSync(app); fs.mkdirSync(service);
  fs.writeFileSync(path.join(app, 'homeplus'), 'binary');
  fs.writeFileSync(path.join(service, 'updates.js'), 'service');
  fs.writeFileSync(path.join(app, 'update-info.json'), 'generated');
  fs.writeFileSync(path.join(app, 'api-token'), 'secret');
  fs.mkdirSync(path.join(service, 'cache')); fs.writeFileSync(path.join(service, 'cache/private'), 'secret');
  const info = provenance.validate(provenance.capture(dir));
  assert.equal(info.source.commit, null); assert.equal(info.upstream.version, null); assert.equal(info.customised, true); assert.equal(info.updater, true);
  assert.deepEqual(provenance.inventory(app, service), {
    'application/homeplus': crypto.createHash('sha256').update('binary').digest('hex'),
    'service/updates.js': crypto.createHash('sha256').update('service').digest('hex')
  });
  assert.equal(JSON.stringify(info).includes(dir), false);
  fs.symlinkSync('/etc/passwd', path.join(service, 'escape'));
  assert.throws(() => provenance.inventory(app, service), /symbolic/);
});
test('rejects malformed metadata and strips unknown builder fields', () => {
  const info = { schemaVersion: 1, upstream: { repository: 'pbalazaar/home-plus', version: '1.0.0', tag: 'v1.0.0', commit: 'a'.repeat(40) },
    source: { repository: 'MoBluey/home-plus', commit: 'b'.repeat(40), base: 'a'.repeat(40) }, customised: true, customisations: ['service/updates.js'], updater: true, workstation: '/private/path' };
  assert.equal(provenance.validate(info).workstation, undefined);
  for (const change of [{ source: { ...info.source, repository: '../../evil' } }, { customisations: ['/private/path'] }, { upstream: { ...info.upstream, tag: 'v9.9.9' } }]) assert.throws(() => provenance.validate({ ...info, ...change }));
});

test('producer excludes the regular application PulseAudio cookie without excluding neighbouring payload', t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-cookie-provenance-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const app = path.join(dir, 'app'), service = path.join(dir, 'service'), cookie = path.join(app, '.config/pulse/cookie');
  fs.mkdirSync(path.dirname(cookie), { recursive: true }); fs.mkdirSync(path.join(service, '.config/pulse'), { recursive: true });
  fs.writeFileSync(cookie, Buffer.alloc(256, 7)); fs.chmodSync(cookie, 0o777);
  fs.writeFileSync(path.join(app, '.config/pulse/extra.js'), 'payload');
  fs.writeFileSync(path.join(service, '.config/pulse/cookie'), 'service payload');
  const read = fs.readFileSync;
  try {
    fs.readFileSync = function (file) { if (file === cookie) throw new Error('Runtime cookie contents must not be read'); return read.apply(this, arguments); };
    const hashes = provenance.inventory(app, service);
    assert.strictEqual(hashes['application/.config/pulse/cookie'], undefined);
    assert.strictEqual(hashes['application/.config/pulse/extra.js'], crypto.createHash('sha256').update('payload').digest('hex'));
    assert.strictEqual(hashes['service/.config/pulse/cookie'], crypto.createHash('sha256').update('service payload').digest('hex'));
    fs.unlinkSync(cookie); fs.symlinkSync(path.join(app, '.config/pulse/extra.js'), cookie);
    assert.throws(() => provenance.inventory(app, service), /symbolic/);
    fs.unlinkSync(cookie); fs.mkdirSync(cookie);
    assert.throws(() => provenance.inventory(app, service), /regular/);
  } finally { fs.readFileSync = read; }
});
