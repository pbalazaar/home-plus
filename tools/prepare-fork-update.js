#!/usr/bin/env node
// Host-side only (Node 16+). Merge trees from a verified upstream base in a private
// clone; never replay old commits or change the caller's index, refs or worktree.
'use strict';
const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const ID = 'com.github.pbalazaar.homeplus';
const RESULT = ID + '.update.json';
const { repository, forkRepository, UPSTREAM, validateArtifact } = require('../service/update-release');
// Source preservation includes the producers/inputs that choose packaged bytes,
// not only the source copied or compiled into the payload.
const buildInput = /^(tools\/(build\.mjs|build-release\.sh|docker-build\.sh|update-provenance\.js|prepare-fork-update\.js)|docker\/(Dockerfile|toolchain\.sha256)|\.github\/workflows\/(ci|fork-updates)\.yml)$/;
const runtimePath = name => buildInput.test(name) || /^(service\/|init\.d\/|native\/(src\/|qml\/|assets\/|img\/|third_party\/|Makefile$|appinfo\.json$)|LICENSE$|THIRD_PARTY_NOTICES\.md$)/.test(name);
function git(repo, args, input) {
  const environment = { ...process.env };
  for (const key of ['GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR', 'GIT_OBJECT_DIRECTORY', 'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_NAMESPACE', 'GIT_PREFIX']) delete environment[key];
  return cp.execFileSync('git', ['-c', 'safe.directory=' + repo, '-c', 'core.hooksPath=/dev/null', '-c', 'commit.gpgSign=false', '-C', repo, ...args], {
    input, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024,
    env: { ...environment, GIT_CONFIG_COUNT: '0', GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_GLOBAL: '/dev/null', GIT_NO_REPLACE_OBJECTS: '1',
      GIT_AUTHOR_NAME: 'Home+ update builder', GIT_AUTHOR_EMAIL: 'builder@invalid', GIT_COMMITTER_NAME: 'Home+ update builder', GIT_COMMITTER_EMAIL: 'builder@invalid' },
    stdio: ['pipe', 'pipe', 'pipe']
  }).trim();
}
function revision(repo, ref) {
  if (typeof ref !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9._/-]*$/.test(ref) || ref.includes('..')) throw new Error('Invalid Git ref');
  return git(repo, ['rev-parse', '--verify', ref + '^{commit}']);
}
// Identity comes from the checkout, never from a manifest or a caller's claim.
function repositoryFromURL(value) {
  const match = /^(?:https:\/\/github\.com\/|git@github\.com:)([^/]+\/[^/]+?)(?:\.git)?$/.exec(value);
  return match && repository(match[1]) ? match[1] : null;
}
function checkoutRepository(repo, source) {
  if (process.env.GITHUB_ACTIONS === 'true') {
    const identity = process.env.GITHUB_REPOSITORY;
    if (!repository(identity) || process.env.GITHUB_SHA !== source || revision(repo, 'HEAD') !== source ||
        repositoryFromURL(git(repo, ['remote', 'get-url', 'origin'])) !== identity) throw new Error('Checkout does not match trusted workflow identity');
    return identity;
  }
  let branches = git(repo, ['for-each-ref', '--format=%(refname:short)', '--points-at', source, 'refs/heads/']).split('\n').filter(Boolean);
  try { if (revision(repo, 'HEAD') === source) branches = [git(repo, ['symbolic-ref', '--quiet', '--short', 'HEAD'])]; } catch (_) { /* Detached source: require unambiguous branch provenance. */ }
  const identities = [];
  for (const branch of branches) {
    let identity;
    try { identity = repositoryFromURL(git(repo, ['remote', 'get-url', git(repo, ['config', 'branch.' + branch + '.remote'])])); }
    catch (_) { continue; }
    if (identity && !identities.includes(identity)) identities.push(identity);
  }
  if (identities.length !== 1) throw new Error('Source revision needs an unambiguous tracking repository');
  return identities[0];
}
function prepare(options) {
  const { repo, destination, version, sourceRepository, upstreamRepository } = options;
  if (!forkRepository(sourceRepository) || upstreamRepository !== UPSTREAM) throw new Error('Invalid repository');
  if (!/^\d+\.\d+\.\d+$/.test(version) || options.upstream !== 'v' + version) throw new Error('Invalid upstream release tag/version');
  if (!path.isAbsolute(repo) || !path.isAbsolute(destination) || fs.existsSync(destination)) throw new Error('Repository/destination must be absolute and destination must not exist');
  const relativeDestination = path.relative(fs.realpathSync(repo), path.join(fs.realpathSync(path.dirname(destination)), path.basename(destination)));
  if (!relativeDestination.startsWith('..' + path.sep) && relativeDestination !== '..' && !path.isAbsolute(relativeDestination)) throw new Error('Candidate must be outside the user repository');
  const source = revision(repo, options.source), upstream = revision(repo, options.upstream), base = revision(repo, options.base);
  if (checkoutRepository(repo, source) !== sourceRepository) throw new Error('Source repository does not match checkout identity');
  const bases = git(repo, ['merge-base', '--all', source, upstream]).split('\n');
  if (bases.length !== 1 || bases[0] !== base) throw new Error('Ambiguous or unverified upstream integration base');
  for (const target of [source, upstream]) git(repo, ['merge-base', '--is-ancestor', base, target]);
  for (const name of ['native/appinfo.json', 'service/package.json']) {
    if (JSON.parse(git(repo, ['show', upstream + ':' + name])).version !== version) throw new Error('Upstream package version does not match release');
  }
  // A cloned object store avoids worktree registration and any writes to the source repo.
  git(repo, ['clone', '--quiet', '--no-checkout', '--no-hardlinks', '--', repo, destination]);
  git(destination, ['checkout', '--quiet', '--detach', upstream]);
  // One synthetic parent anchors the COMPLETE tracked source delta to its verified
  // base. Git can drop identical hunks already upstream, but real overlaps conflict.
  const tree = git(destination, ['rev-parse', source + '^{tree}']);
  const delta = git(destination, ['commit-tree', tree, '-p', base], 'Managed fork source\n');
  const result = {
    schemaVersion: 1,
    upstream: { repository: upstreamRepository, version, tag: 'v' + version, commit: upstream },
    source: { repository: sourceRepository, commit: source, base },
    status: 'failed', message: 'Candidate checks have not completed.', conflicts: [], customisations: []
  };
  try { git(destination, ['merge', '--no-commit', '--no-ff', delta]); }
  catch (error) {
    const conflicts = git(destination, ['diff', '--name-only', '--diff-filter=U']).split('\n').filter(Boolean);
    if (!conflicts.length) throw error;
    result.status = 'conflict'; result.message = 'Tracked custom source conflicts with this upstream release.'; result.conflicts = conflicts;
    result.customisations = git(repo, ['diff', '--name-only', base, source]).split('\n').filter(runtimePath);
    fs.writeFileSync(path.join(destination, RESULT), JSON.stringify(result, null, 2) + '\n');
    return result;
  }
  const mergedTree = git(destination, ['write-tree']);
  const candidate = git(destination, ['commit-tree', mergedTree, '-p', upstream, '-p', source], 'Checked fork update candidate\n');
  git(destination, ['reset', '--quiet', '--hard', candidate]);
  result.customisations = git(destination, ['diff', '--name-only', upstream, candidate]).split('\n').filter(runtimePath);
  const provenance = { schemaVersion: 1, upstream: result.upstream, source: result.source,
    customised: result.customisations.length > 0, customisations: result.customisations,
    updater: fs.existsSync(path.join(destination, 'service/updates.js')) };
  fs.writeFileSync(path.join(destination, 'update-info-build.json'), JSON.stringify(provenance, null, 2) + '\n');
  fs.writeFileSync(path.join(destination, RESULT), JSON.stringify(result, null, 2) + '\n');
  return result;
}
function finish(result, candidate, passed, official) {
  if (!result.source || !forkRepository(result.source.repository) || !result.upstream || result.upstream.repository !== UPSTREAM) throw new Error('Invalid result repository');
  if (result.status === 'conflict') throw new Error('Cannot finish a conflicted candidate');
  const completed = { ...result, status: 'failed', message: 'Candidate checks failed.' }; delete completed.artifact;
  if (!passed) return completed;
  if (!result.customisations.length) {
    const filename = ID + '_' + result.upstream.version + '_all.ipk';
    const url = 'https://github.com/pbalazaar/home-plus/releases/download/' + result.upstream.tag + '/' + filename;
    if (!official || official.url !== url || official.version !== result.upstream.version || !/^[a-f0-9]{64}$/.test(official.sha256) || !Number.isSafeInteger(official.size) || official.size < 1) throw new Error('Verified official package required when runtime changes are absorbed');
    completed.status = 'ready'; completed.message = 'All runtime customisations are exactly present upstream.'; // Omit artifact: the controller selects the independently verified official release asset.
    return completed;
  }
  const manifestFile = path.join(candidate, 'dist', ID + '.manifest.json');
  if (!fs.existsSync(manifestFile)) throw new Error('Candidate package manifest is missing');
  const manifest = JSON.parse(fs.readFileSync(manifestFile));
  const filename = ID + '_' + result.upstream.version + '_all.ipk';
  const packageFile = path.join(candidate, 'dist', filename);
  if (manifest.id !== ID || manifest.version !== result.upstream.version || manifest.ipkUrl !== filename || !fs.existsSync(packageFile)) throw new Error('Invalid candidate package');
  const data = fs.readFileSync(packageFile), hash = crypto.createHash('sha256').update(data).digest('hex');
  if (manifest.ipkSize !== data.length || !manifest.ipkHash || manifest.ipkHash.sha256 !== hash) throw new Error('Candidate package hash/size mismatch');
  completed.status = 'ready'; completed.message = 'Custom source integrated and all required checks passed.';
  const immutableName = ID + '_' + result.upstream.version + '_' + result.source.commit.slice(0, 12) + '_' + hash.slice(0, 12) + '_all.ipk';
  completed.artifact = { url: 'https://github.com/' + result.source.repository + '/releases/download/upstream-v' + result.upstream.version + '/' + immutableName, sha256: hash, size: data.length, version: result.upstream.version };
  validateArtifact(completed.artifact, result.source.repository, 'upstream-v' + result.upstream.version, result.source.commit);
  return completed;
}
function sameCandidate(previous, source, upstream, sourceRepository) {
  return forkRepository(sourceRepository) && !!previous && previous.schemaVersion === 1 && ['ready', 'failed', 'conflict'].includes(previous.status) && previous.source && previous.source.repository === sourceRepository && previous.source.commit === source && previous.upstream && previous.upstream.repository === UPSTREAM && previous.upstream.commit === upstream;
}
module.exports = { prepare, finish, sameCandidate, runtimePath, repository, repositoryFromURL, checkoutRepository, RESULT };
if (require.main === module) {
  try {
    const [command, ...values] = process.argv.slice(2);
    if (command === 'prepare') {
      const [repo, destination, source, base, version, sourceRepository] = values;
      console.log(JSON.stringify(prepare({ repo, destination, source, base, version, sourceRepository, upstream: 'v' + version, upstreamRepository: 'pbalazaar/home-plus' })));
    } else if (command === 'finish') {
      const [file, candidate, passed] = values;
      const result = JSON.parse(fs.readFileSync(file));
      fs.writeFileSync(file, JSON.stringify(finish(result, candidate, passed === 'true'), null, 2) + '\n');
    } else throw new Error('Usage: prepare-fork-update.js prepare <repo> <new-directory> <source-ref> <verified-base> <version> <fork-owner/repo> | finish <result-file> <candidate> <true|false>');
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
