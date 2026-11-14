'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const https = require('https'), http = require('http'), { URL } = require('url');
const release = require('../service/update-release'), { mockServer } = require('./helpers');
test('production release reader validates a controlled HTTP feed and official routing never requests the fork', async () => {
  const APP = release.APP, version = '0.26.0', tag = 'v' + version;
  const manifestName = APP + '.manifest.json', ipkName = APP + '_' + version + '_all.ipk';
  const prefix = '/pbalazaar/home-plus/releases/download/' + tag + '/';
  const asset = (name, size) => ({ name, size, browser_download_url: 'https://github.com' + prefix + name });
  const r = { draft: false, prerelease: false, tag_name: tag, body: 'A release', assets: [asset(manifestName, 512), asset(ipkName, 100)] };
  const manifest = { id: APP, version, ipkUrl: ipkName, ipkSize: 100, ipkHash: { sha256: 'b'.repeat(64) } };
  let mode = 'normal';
  const server = await mockServer((req, u) => {
    if (mode === 'rate') return { status: 403, json: {} };
    if (mode === 'redirect') return { status: 302, headers: { Location: 'https://evil.example/payload' } };
    if (u.pathname === '/repos/pbalazaar/home-plus/releases/latest') return { json: r };
    if (u.pathname === prefix + manifestName) return { json: manifest };
    if (u.pathname === '/repos/pbalazaar/home-plus/git/ref/tags/' + tag) return { json: { object: mode === 'tag-trick' ? { type: 'tag', sha: '../../Other/fork' } : { type: 'commit', sha: 'a'.repeat(40) } } };
  });
  const previous = https.request, base = new URL(server.base);
  https.request = (options, callback) => http.request(Object.assign({}, options, { protocol: 'http:', hostname: '127.0.0.1', port: base.port }), callback);
  try {
    const result = await release.fetchRelease(null); assert.strictEqual(result.upstream.version, version); assert.strictEqual(result.result, null);
    assert.strictEqual(server.hits.length, 3); assert(server.hits.every(h => h.path.indexOf('MoBluey') === -1));
    assert(server.hits.every(h => !h.path.endsWith('.ipk')));
    mode = 'tag-trick'; await assert.rejects(release.fetchRelease(null), /tag object/);
    assert(!server.hits.some(h => h.path.indexOf('Other/fork') !== -1));
    mode = 'rate'; await assert.rejects(release.fetchRelease(null), /HTTP 403/);
    mode = 'redirect'; await assert.rejects(release.download('https://github.com/pbalazaar/home-plus', 1024), /Untrusted/);
  } finally { https.request = previous; await server.close(); }
});

test('repository identities and custom artifact paths are strict and portable', () => {
  for (const repo of ['MoBluey/home-plus', 'Example-Owner/custom.home-plus']) {
    assert.strictEqual(release.repository(repo), true);
    const artifact = { version: '1.2.3', size: 3, sha256: 'b'.repeat(64), url: 'https://github.com/' + repo + '/releases/download/upstream-v1.2.3/' + release.APP + '_1.2.3_aaaaaaaaaaaa_bbbbbbbbbbbb_all.ipk' };
    assert.strictEqual(release.validateArtifact(artifact, repo, 'upstream-v1.2.3', 'a'.repeat(40)), artifact);
    for (const url of [artifact.url.replace(repo, 'Other/fork'), artifact.url.replace('/releases/', '/x/../releases/'), artifact.url + '?x=1', artifact.url.replace('/upstream-v', '/v')])
      assert.throws(() => release.validateArtifact(Object.assign({}, artifact, { url }), repo, 'upstream-v1.2.3', 'a'.repeat(40)));
  }
  for (const repo of [null, '', 'owner', 'owner/repo/extra', '../repo', 'owner/..', 'owner/a..b', 'owner/repo.git', 'owner/repo?x', 'owner/repo#x', 'owner/%2e%2e', 'owner\\repo', '-owner/repo', 'owner_/repo', 'owner/repo\n']) assert.strictEqual(release.repository(repo), false, String(repo));
});

test('release reader uses only the designated fork API and exact result asset URL', async () => {
  const APP = release.APP, version = '1.2.3', tag = 'v' + version;
  const upstreamPrefix = '/pbalazaar/home-plus/releases/download/' + tag + '/';
  const manifestName = APP + '.manifest.json', ipkName = APP + '_' + version + '_all.ipk';
  const asset = (name, prefix, size) => ({ name, size, browser_download_url: 'https://github.com' + prefix + name });
  let fork = 'MoBluey/home-plus', badAsset = false;
  const result = { marker: 'custom result' };
  const server = await mockServer((req, u) => {
    if (u.pathname === '/repos/pbalazaar/home-plus/releases/latest') return { json: { draft: false, prerelease: false, tag_name: tag, assets: [asset(manifestName, upstreamPrefix, 512), asset(ipkName, upstreamPrefix, 100)] } };
    if (u.pathname === upstreamPrefix + manifestName) return { json: { id: APP, version, ipkUrl: ipkName, ipkSize: 100, ipkHash: { sha256: 'b'.repeat(64) } } };
    if (u.pathname === '/repos/pbalazaar/home-plus/git/ref/tags/' + tag) return { json: { object: { type: 'commit', sha: 'a'.repeat(40) } } };
    if (u.pathname === '/repos/' + fork + '/releases/tags/upstream-v' + version) return { json: { draft: false, prerelease: false, tag_name: 'upstream-v' + version, assets: [asset(APP + '.update.json', '/' + (badAsset ? 'Other/fork' : fork) + '/releases/download/upstream-v' + version + '/', 512)] } };
    if (u.pathname === '/' + fork + '/releases/download/upstream-v' + version + '/' + APP + '.update.json') return { json: result };
  });
  const previous = https.request, base = new URL(server.base);
  https.request = (options, callback) => http.request(Object.assign({}, options, { protocol: 'http:', hostname: '127.0.0.1', port: base.port }), callback);
  try {
    for (const repository of ['MoBluey/home-plus', 'Example-Owner/custom.home-plus']) {
      fork = repository; badAsset = false;
      const start = server.hits.length;
      assert.deepStrictEqual((await release.fetchRelease(fork)).result, result);
      assert.strictEqual(server.hits.length - start, 5);
      assert(server.hits.slice(start).some(hit => hit.path === '/repos/' + fork + '/releases/tags/upstream-v' + version));
      badAsset = true;
      assert.strictEqual((await release.fetchRelease(fork)).result, null);
      assert(!server.hits.some(hit => hit.path.indexOf('Other/fork') !== -1));
    }
    const count = server.hits.length;
    for (const identity of ['../fork', 'owner/repo?x', 'pbalazaar/home-plus', 'PBALAZAAR/home-plus']) await assert.rejects(release.fetchRelease(identity), /repository/);
    assert.strictEqual(server.hits.length, count);
  } finally { https.request = previous; await server.close(); }
});
