/* Fixed upstream and a packaged-provenance-bound fork; never request-supplied feeds. */
'use strict';
const https = require('https');
const { URL } = require('url');
const { requestOptions } = require('./net');
const APP = 'com.github.pbalazaar.homeplus';
const UPSTREAM = 'pbalazaar/home-plus';
const repository = value => typeof value === 'string' && /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?\/[A-Za-z0-9_.-]{1,100}$/.test(value) && !value.includes('..') && !/\.git$/i.test(value) && value.split('/')[1] !== '.';
const forkRepository = value => repository(value) && value.toLowerCase() !== UPSTREAM;
const MAX_IPK = 128 * 1024 * 1024;
const plain = (value, limit) => String(value || '').replace(/[\x00-\x08\x0b-\x1f\x7f]/g, '').slice(0, limit || 4000);
function version(value) {
  if (typeof value !== 'string' || !/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(value) || value.split('.').some(n => Number(n) > 999999)) throw new Error('Invalid stable version');
  return value.split('.').map(Number);
}
function compareVersions(a, b) { const x = version(a), y = version(b); for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1; return 0; }
function validURL(value, redirects) {
  const u = new URL(value);
  if (u.protocol !== 'https:' || u.username || u.password || u.port || u.hash ||
      !(['api.github.com', 'github.com'].concat(redirects ? ['release-assets.githubusercontent.com', 'objects.githubusercontent.com'] : []).includes(u.hostname))) throw new Error('Untrusted update URL');
  return u;
}
function validateArtifact(a, repo, tag, sourceCommit) {
  if (!a || !repository(repo) || (repo !== UPSTREAM && !forkRepository(repo))) throw new Error('Invalid update artifact');
  version(a.version);
  if (tag !== (repo === UPSTREAM ? 'v' : 'upstream-v') + a.version) throw new Error('Invalid update artifact tag');
  const u = validURL(a.url, false);
  const suffix = repo !== UPSTREAM ? '_' + String(sourceCommit || '').slice(0, 12) + '_' + String(a.sha256 || '').slice(0, 12) : '';
  if (repo !== UPSTREAM && !/^[0-9a-f]{40}$/.test(sourceCommit)) throw new Error('Missing custom build source revision');
  const expected = '/' + repo + '/releases/download/' + tag + '/' + APP + '_' + a.version + suffix + '_all.ipk';
  if (u.hostname !== 'github.com' || a.url !== 'https://github.com' + expected || u.pathname !== expected || u.search || !/^[0-9a-f]{64}$/.test(a.sha256) || !Number.isInteger(a.size) || a.size < 1 || a.size > MAX_IPK) throw new Error('Invalid update artifact identity, size or hash');
  return a;
}
function download(url, limit) {
  const deadline = Date.now() + 30000;
  function step(address, remaining) {
    return new Promise((resolve, reject) => {
      let u; try { u = validURL(address, true); } catch (e) { reject(e); return; }
      const req = https.request(requestOptions(u, { method: 'GET', headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'HomePlus-Updater' } }), res => {
        if ([301, 302, 303, 307, 308].includes(res.statusCode)) {
          res.resume(); clearTimeout(timer);
          if (!remaining || !res.headers.location) return reject(new Error('Too many update redirects'));
          return step(new URL(res.headers.location, u).href, remaining - 1).then(resolve, reject);
        }
        if (res.statusCode !== 200) { res.resume(); clearTimeout(timer); return reject(new Error('Release check returned HTTP ' + res.statusCode)); }
        const chunks = []; let bytes = 0;
        res.on('data', c => { bytes += c.length; if (bytes > limit) req.destroy(new Error('Update download exceeds size limit')); else chunks.push(c); });
        res.on('end', () => { clearTimeout(timer); if (res.complete === false) return reject(new Error('Incomplete update download')); resolve(Buffer.concat(chunks)); });
        res.on('error', reject);
        res.on('close', () => { if (res.complete === false) reject(new Error('Incomplete update download')); });
      });
      const timer = setTimeout(() => req.destroy(new Error('Update request timed out')), Math.max(1, deadline - Date.now()));
      req.on('error', e => { clearTimeout(timer); reject(e); }); req.end();
    });
  }
  return step(url, 4);
}
const json = async url => JSON.parse((await download(url, 1024 * 1024)).toString('utf8'));
function asset(release, name, repo) {
  const a = (release.assets || []).filter(x => x.name === name);
  if (a.length !== 1 || a[0].browser_download_url !== 'https://github.com/' + repo + '/releases/download/' + release.tag_name + '/' + name) throw new Error('Release asset is missing or ambiguous');
  return a[0];
}
async function fetchRelease(fork) {
  if (fork !== null && fork !== undefined && !forkRepository(fork)) throw new Error('Invalid managed fork repository');
  const r = await json('https://api.github.com/repos/' + UPSTREAM + '/releases/latest');
  if (!r || r.draft !== false || r.prerelease !== false || typeof r.tag_name !== 'string' || r.tag_name[0] !== 'v') throw new Error('No stable upstream release');
  const v = r.tag_name.slice(1); version(v);
  const manifest = asset(r, APP + '.manifest.json', UPSTREAM);
  const m = await json(manifest.browser_download_url);
  if (m.id !== APP || m.version !== v || m.ipkUrl !== APP + '_' + v + '_all.ipk') throw new Error('Upstream manifest identity mismatch');
  const ipk = asset(r, m.ipkUrl, UPSTREAM);
  const artifact = validateArtifact({ url: ipk.browser_download_url, size: m.ipkSize, sha256: m.ipkHash && m.ipkHash.sha256, version: v }, UPSTREAM, r.tag_name);
  if (ipk.size !== artifact.size) throw new Error('Upstream asset size mismatch');
  let ref = await json('https://api.github.com/repos/' + UPSTREAM + '/git/ref/tags/' + r.tag_name);
  for (let i = 0; ref.object && ref.object.type === 'tag' && i < 3; i++) {
    if (!/^[0-9a-f]{40}$/.test(ref.object.sha)) throw new Error('Invalid release tag object');
    ref = await json('https://api.github.com/repos/' + UPSTREAM + '/git/tags/' + ref.object.sha);
  }
  if (!ref.object || ref.object.type !== 'commit' || !/^[0-9a-f]{40}$/.test(ref.object.sha)) throw new Error('Cannot resolve release commit');
  const upstream = { repository: UPSTREAM, version: v, tag: r.tag_name, commit: ref.object.sha, artifact, notes: plain(r.body) };
  if (!fork) return { upstream, result: null };
  let result = null, resultMessage = '';
  try {
    const custom = await json('https://api.github.com/repos/' + fork + '/releases/tags/upstream-v' + v);
    if (custom.draft !== false || custom.prerelease !== false || custom.tag_name !== 'upstream-v' + v) throw new Error('Custom build is not published');
    result = await json(asset(custom, APP + '.update.json', fork).browser_download_url);
  } catch (e) { resultMessage = 'Custom build result is unavailable. ' + plain(e.message, 200); }
  return { upstream, result, resultMessage };
}
module.exports = { APP, UPSTREAM, repository, forkRepository, MAX_IPK, plain, version, compareVersions, validURL, validateArtifact, download, fetchRelease };
