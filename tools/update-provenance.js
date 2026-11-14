// Build-time provenance only; no paths, environment, credentials or Git URLs ship.
'use strict';
const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const { RUNTIME_COOKIE } = require('../service/update-archive');
const { repository, runtimePath, checkoutRepository } = require('./prepare-fork-update');
function git(repo, args) { return cp.execFileSync('git', ['-c', 'safe.directory=' + repo, '-C', repo, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim(); }
function capture(repo) {
  const result = { schemaVersion: 1, upstream: { repository: 'pbalazaar/home-plus', version: null, tag: null, commit: null },
    source: { repository: null, commit: null, base: null }, customised: true, customisations: [], updater: fs.existsSync(path.join(repo, 'service/updates.js')) };
  try {
    // Dirty or exported source cannot be attributed to a committed revision.
    if (git(repo, ['status', '--porcelain', '--untracked-files=normal']).split('\n').some(line => line && !/^(\?\?) (update-info-build\.json|com\.github\.pbalazaar\.homeplus\.update\.json)$/.test(line))) return result;
    const commit = git(repo, ['rev-parse', 'HEAD']);
    const sourceRepo = checkoutRepository(repo, commit);
    const ciSource = process.env.GITHUB_ACTIONS === 'true';
    if (!sourceRepo) return result;
    result.source.repository = sourceRepo; result.source.commit = commit;
    if (ciSource && sourceRepo === 'pbalazaar/home-plus' && /^refs\/tags\/v\d+\.\d+\.\d+$/.test(process.env.GITHUB_REF || '')) {
      const tag = process.env.GITHUB_REF.slice('refs/tags/'.length);
      if (JSON.parse(fs.readFileSync(path.join(repo, 'native/appinfo.json'))).version !== tag.slice(1) || git(repo, ['rev-parse', tag + '^{commit}']) !== commit) return result;
      result.upstream = { repository: sourceRepo, version: tag.slice(1), tag, commit };
      result.source.base = commit; result.customised = false; return result;
    }
    // Local tags and remote URLs do not prove an official release identity.
    // Fork workflows pass independently verified provenance from prepare(); desktop
    // builds retain known source identity but explicitly unknown upstream/base.
  } catch (_) { /* No Git, branch/upstream or trustworthy origin: explicitly unknown. */ }
  return result;
}
function validate(info) {
  if (!info || info.schemaVersion !== 1 || !info.upstream || info.upstream.repository !== 'pbalazaar/home-plus' || !info.source ||
      typeof info.customised !== 'boolean' || typeof info.updater !== 'boolean' || !Array.isArray(info.customisations)) throw new Error('Invalid update provenance');
  for (const value of [info.upstream.commit, info.source.commit, info.source.base]) if (value !== null && !/^[0-9a-f]{40}$/.test(value)) throw new Error('Invalid provenance commit');
  if (info.source.repository !== null && !repository(info.source.repository)) throw new Error('Invalid provenance repository');
  if (info.upstream.version !== null && (!/^\d+\.\d+\.\d+$/.test(info.upstream.version) || info.upstream.tag !== 'v' + info.upstream.version)) throw new Error('Invalid provenance release');
  for (const name of info.customisations) if (typeof name !== 'string' || !runtimePath(name) || name.includes('..') || name.includes('\\') || /[\x00-\x1f]/.test(name)) throw new Error('Invalid customisation path');
  // Select fields rather than copying arbitrary builder metadata into the package.
  return { schemaVersion: 1, upstream: { repository: info.upstream.repository, version: info.upstream.version, tag: info.upstream.tag, commit: info.upstream.commit },
    source: { repository: info.source.repository, commit: info.source.commit, base: info.source.base }, customised: info.customised, customisations: info.customisations, updater: info.updater };
}
function inventory(application, service) {
  const hashes = {};
  function walk(dir, prefix) {
    for (const name of fs.readdirSync(dir).sort()) {
      if (name === 'update-info.json' || name === 'api-token' || name === 'cache') continue;
      const file = path.join(dir, name), key = prefix + '/' + name, st = fs.lstatSync(file);
      if (st.isSymbolicLink()) throw new Error('Package payload must not contain symbolic links');
      if (key === 'application/' + RUNTIME_COOKIE) {
        if (!st.isFile()) throw new Error('Runtime cookie must be a regular file');
        continue;
      }
      if (st.isDirectory()) walk(file, key);
      else if (st.isFile()) hashes[key] = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
      else throw new Error('Package payload must contain regular files');
    }
  }
  walk(application, 'application'); walk(service, 'service'); return hashes;
}
module.exports = { capture, validate, inventory };
if (require.main === module) process.stdout.write(JSON.stringify(capture(path.dirname(__dirname)), null, 2) + '\n');
