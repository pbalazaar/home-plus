'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');
const { prepare, finish } = require('../tools/prepare-fork-update');
const ciNames = ['GITHUB_ACTIONS', 'GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_REF', 'GH_REPO'];
const ciEnvironment = ciNames.map(name => process.env[name]);
test.beforeEach(() => ciNames.forEach(name => { delete process.env[name]; }));
test.afterEach(() => ciNames.forEach((name, i) => { if (ciEnvironment[i] === undefined) delete process.env[name]; else process.env[name] = ciEnvironment[i]; }));
function git(repo, ...args) { return cp.execFileSync('git', ['-C', repo, ...args], { encoding: 'utf8' }).trim(); }
function fixture(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-fork-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const repo = path.join(dir, 'repo'); fs.mkdirSync(repo);
  git(repo, 'init', '-q'); git(repo, 'config', 'user.email', 'fixture@example.invalid'); git(repo, 'config', 'user.name', 'Fixture');
  function commit(files, message) {
    for (const [name, value] of Object.entries(files)) { fs.mkdirSync(path.dirname(path.join(repo, name)), { recursive: true }); fs.writeFileSync(path.join(repo, name), value); }
    git(repo, 'add', '.'); git(repo, 'commit', '-qm', message); return git(repo, 'rev-parse', 'HEAD');
  }
  const base = commit({ 'service/a.js': 'base\n', 'native/appinfo.json': '{"version":"1.0.0"}', 'service/package.json': '{"version":"1.0.0"}' }, 'base');
  const source = commit({ 'service/custom.js': 'preserved\n' }, 'custom');
  git(repo, 'remote', 'add', 'origin', 'https://github.com/MoBluey/home-plus.git');
  git(repo, 'branch', 'source-identity', source); git(repo, 'config', 'branch.source-identity.remote', 'origin');
  git(repo, 'checkout', '-q', '--detach', base);
  const upstream = commit({ 'service/new.js': 'upstream\n', 'native/appinfo.json': '{"version":"1.1.0"}', 'service/package.json': '{"version":"1.1.0"}' }, 'release');
  git(repo, 'tag', 'v1.1.0');
  const options = { repo, destination: path.join(dir, 'candidate'), base, source, upstream: 'v1.1.0', version: '1.1.0', sourceRepository: 'MoBluey/home-plus', upstreamRepository: 'pbalazaar/home-plus' };
  return { dir, repo, base, source, upstream, commit, options };
}
test('integrates tracked source without changing dirty user checkout or refs; replay is deterministic', t => {
  const f = fixture(t); fs.writeFileSync(path.join(f.repo, 'dirty'), 'private');
  const before = git(f.repo, 'show-ref'); const head = git(f.repo, 'rev-parse', 'HEAD');
  const result = prepare(f.options);
  assert.equal(result.status, 'failed'); assert.match(result.message, /checks/i);
  const info = require('../tools/update-provenance').validate(JSON.parse(fs.readFileSync(path.join(f.options.destination, 'update-info-build.json'))));
  assert.equal(info.upstream.commit, f.upstream); assert.equal(info.source.commit, f.source); assert.equal(info.source.base, f.base); assert.equal(info.customised, true);
  assert.equal(fs.readFileSync(path.join(f.options.destination, 'service/custom.js'), 'utf8'), 'preserved\n');
  assert.equal(fs.readFileSync(path.join(f.options.destination, 'service/new.js'), 'utf8'), 'upstream\n');
  assert.equal(git(f.repo, 'show-ref'), before); assert.equal(git(f.repo, 'rev-parse', 'HEAD'), head);
  assert.equal(fs.readFileSync(path.join(f.repo, 'dirty'), 'utf8'), 'private');
  assert.deepEqual(prepare({ ...f.options, destination: path.join(f.dir, 'again') }).customisations, result.customisations);
  assert.throws(() => prepare(f.options), /exist/);
});
test('exact changes already upstream disappear without semantic guesses', t => {
  const f = fixture(t); f.commit({ 'service/custom.js': 'preserved\n' }, 'same change upstream');
  git(f.repo, 'tag', '-f', 'v1.1.0');
  const result = prepare(f.options);
  assert.deepEqual(result.customisations, []);
});
test('reports genuine source conflicts and never packages them', t => {
  const f = fixture(t); const upstream = f.commit({ 'service/a.js': 'upstream changed\n' }, 'upstream conflict'); git(f.repo, 'tag', '-f', 'v1.1.0');
  git(f.repo, 'checkout', '-q', '--detach', f.source);
  const source = f.commit({ 'service/a.js': 'fork changed\n' }, 'fork conflict');
  git(f.repo, 'branch', '-f', 'source-identity', source);
  const result = prepare({ ...f.options, source });
  assert.equal(result.status, 'conflict'); assert.deepEqual(result.conflicts, ['service/a.js']);
  assert.throws(() => finish(result, f.options.destination, true), /conflict/);
  assert.equal(git(f.repo, 'rev-parse', 'v1.1.0'), upstream);
});
test('rejects invalid repositories, refs, version claims and unrelated base', t => {
  const f = fixture(t);
  for (const change of [{ sourceRepository: '../../evil' }, { upstream: '--help' }, { source: 'missing' }, { base: f.upstream }, { version: '9.0.0' }, { destination: path.join(f.repo, 'candidate') }]) {
    assert.throws(() => prepare({ ...f.options, ...change }));
    assert.equal(fs.existsSync(f.options.destination), false);
  }
});
test('failed checks cannot produce a ready result', t => {
  const f = fixture(t); const result = prepare(f.options);
  const failed = finish(result, f.options.destination, false);
  assert.equal(failed.status, 'failed'); assert.equal(failed.artifact, undefined);
  assert.throws(() => finish(result, f.options.destination, true), /package/i);
});
test('ready result verifies bytes and uses immutable source/hash asset name; hash drift fails', t => {
  const f = fixture(t); const result = prepare(f.options);
  const dist = path.join(f.options.destination, 'dist'); fs.mkdirSync(dist);
  const data = Buffer.from('checked package fixture'), sha = require('node:crypto').createHash('sha256').update(data).digest('hex');
  const name = 'com.github.pbalazaar.homeplus_1.1.0_all.ipk';
  fs.writeFileSync(path.join(dist, name), data);
  const manifest = { id: 'com.github.pbalazaar.homeplus', version: '1.1.0', ipkUrl: name, ipkSize: data.length, ipkHash: { sha256: sha } };
  fs.writeFileSync(path.join(dist, 'com.github.pbalazaar.homeplus.manifest.json'), JSON.stringify(manifest));
  const ready = finish(result, f.options.destination, true);
  assert.equal(ready.status, 'ready'); assert.equal(ready.artifact.sha256, sha);
  assert.equal(ready.artifact.url, 'https://github.com/MoBluey/home-plus/releases/download/upstream-v1.1.0/com.github.pbalazaar.homeplus_1.1.0_' + f.source.slice(0, 12) + '_' + sha.slice(0, 12) + '_all.ipk');
  const { sameCandidate } = require('../tools/prepare-fork-update');
  assert.equal(sameCandidate(ready, f.source, f.upstream, 'MoBluey/home-plus'), true);
  assert.equal(sameCandidate(ready, 'a'.repeat(40), f.upstream, 'MoBluey/home-plus'), false);
  fs.appendFileSync(path.join(dist, name), 'changed'); assert.throws(() => finish(result, f.options.destination, true), /hash\/size/);
});
test('all runtime changes absorbed requires the verified official package', t => {
  const f = fixture(t); f.commit({ 'service/custom.js': 'preserved\n' }, 'same upstream'); git(f.repo, 'tag', '-f', 'v1.1.0');
  const result = prepare(f.options);
  assert.throws(() => finish(result, f.options.destination, true), /official package/);
  const official = { url: 'https://github.com/pbalazaar/home-plus/releases/download/v1.1.0/com.github.pbalazaar.homeplus_1.1.0_all.ipk', sha256: 'a'.repeat(64), size: 1, version: '1.1.0' };
  assert.equal(finish(result, f.options.destination, true, official).artifact, undefined);
  assert.throws(() => finish(result, f.options.destination, true, { ...official, url: 'https://evil.invalid/a.ipk' }), /official package/);
});
test('trusted finaliser rejects identity drift and records package/check failure honestly', t => {
  const f = fixture(t); const result = prepare(f.options);
  const { checkedResult, finalise } = require('../tools/publish-fork-update');
  const expected = { repository: 'MoBluey/home-plus', source: f.source, version: '1.1.0' };
  assert.throws(() => checkedResult({ ...result, source: { ...result.source, commit: 'f'.repeat(40) } }, expected), /identity/);
  const file = path.join(f.dir, 'result.json'); fs.writeFileSync(file, JSON.stringify(result));
  const failed = finalise(f.options.destination, file, expected, true);
  assert.equal(failed.status, 'failed'); assert.equal(failed.artifact, undefined); assert.match(failed.message, /verification failed/);
  assert.equal(JSON.stringify(failed).includes(f.dir), false);
});
test('official detached CI build records verified release, while dirty origin stays unknown', t => {
  const f = fixture(t), { capture } = require('../tools/update-provenance');
  const names = ['GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_REF', 'GITHUB_ACTIONS'], old = names.map(name => process.env[name]);
  try {
    git(f.repo, 'remote', 'set-url', 'origin', 'https://github.com/pbalazaar/home-plus.git');
    process.env.GITHUB_ACTIONS = 'true'; process.env.GITHUB_REPOSITORY = 'pbalazaar/home-plus'; process.env.GITHUB_SHA = f.upstream; process.env.GITHUB_REF = 'refs/tags/v1.1.0';
    const official = capture(f.repo); assert.equal(official.customised, false); assert.equal(official.upstream.commit, f.upstream); assert.equal(official.source.commit, f.upstream);
    process.env.GITHUB_REPOSITORY = 'Other/fork';
    assert.equal(capture(f.repo).source.repository, null);
    process.env.GITHUB_REPOSITORY = 'pbalazaar/home-plus';
    process.env.GITHUB_SHA = f.source; assert.equal(capture(f.repo).source.repository, null); process.env.GITHUB_SHA = f.upstream;
    fs.writeFileSync(path.join(f.repo, 'private'), 'untracked modification');
    const dirty = capture(f.repo); assert.equal(dirty.source.commit, null); assert.equal(dirty.customised, true);
  } finally { names.forEach((name, i) => { if (old[i] === undefined) delete process.env[name]; else process.env[name] = old[i]; }); }
});
test('inherited Git index/worktree controls cannot redirect candidate writes into user checkout', t => {
  const f = fixture(t), before = fs.readFileSync(path.join(f.repo, '.git/index'));
  const names = ['GIT_WORK_TREE', 'GIT_INDEX_FILE'], old = names.map(name => process.env[name]);
  try {
    process.env.GIT_WORK_TREE = f.repo; process.env.GIT_INDEX_FILE = path.join(f.repo, '.git/index');
    const result = prepare(f.options); assert.equal(result.status, 'failed');
    assert.deepEqual(fs.readFileSync(path.join(f.repo, '.git/index')), before);
    assert.equal(fs.existsSync(path.join(f.repo, 'service/custom.js')), false);
  } finally { names.forEach((name, i) => { if (old[i] === undefined) delete process.env[name]; else process.env[name] = old[i]; }); }
});
test('build-only retained producer changes publish the custom payload rather than official fallback', t => {
  const f = fixture(t), crypto = require('node:crypto');
  git(f.repo, 'checkout', '-q', '--detach', f.base);
  const builder = payload => 'import fs from "node:fs"; import crypto from "node:crypto"; fs.mkdirSync("dist",{recursive:true}); const data=Buffer.from(' + JSON.stringify(payload) + '); const name="com.github.pbalazaar.homeplus_1.1.0_all.ipk"; fs.writeFileSync("dist/"+name,data); fs.writeFileSync("dist/com.github.pbalazaar.homeplus.manifest.json",JSON.stringify({id:"com.github.pbalazaar.homeplus",version:"1.1.0",ipkUrl:name,ipkSize:data.length,ipkHash:{sha256:crypto.createHash("sha256").update(data).digest("hex")}}));';
  const base = f.commit({ 'tools/build.mjs': builder('official payload') }, 'base builder');
  const source = f.commit({ 'tools/build.mjs': builder('preserved custom payload') }, 'custom build producer only');
  git(f.repo, 'checkout', '-q', '--detach', base);
  f.commit({ 'native/appinfo.json': '{"version":"1.1.0"}', 'service/package.json': '{"version":"1.1.0"}' }, 'new official release'); git(f.repo, 'tag', '-f', 'v1.1.0');
  git(f.repo, 'branch', '-f', 'source-identity', source);
  const result = prepare({ ...f.options, base, source });
  // Run the retained producer through the same Node entry point as production.
  cp.execFileSync(process.execPath, ['tools/build.mjs'], { cwd: f.options.destination });
  const data = fs.readFileSync(path.join(f.options.destination, 'dist/com.github.pbalazaar.homeplus_1.1.0_all.ipk'));
  assert.equal(data.toString(), 'preserved custom payload');
  assert.deepEqual(result.customisations, ['tools/build.mjs']);
  const ready = finish(result, f.options.destination, true);
  assert.equal(ready.artifact.sha256, crypto.createHash('sha256').update(data).digest('hex'));
  const { runtimePath } = require('../tools/prepare-fork-update');
  for (const name of ['tools/build-release.sh', 'tools/docker-build.sh', 'tools/update-provenance.js', 'tools/prepare-fork-update.js', 'docker/Dockerfile', 'docker/toolchain.sha256', '.github/workflows/ci.yml', '.github/workflows/fork-updates.yml']) assert.equal(runtimePath(name), true, name);
  assert.equal(runtimePath('docs/screenshots/old.png'), false);
});
test('fork-only reachable version tag cannot claim an official upstream base', t => {
  const f = fixture(t), { capture } = require('../tools/update-provenance');
  git(f.repo, 'checkout', '-q', '-b', 'fork-source', f.source);
  git(f.repo, 'remote', 'add', 'fork', 'https://github.com/MoBluey/home-plus.git');
  git(f.repo, 'remote', 'add', 'upstream', 'https://github.com/pbalazaar/home-plus.git');
  git(f.repo, 'config', 'branch.fork-source.remote', 'fork');
  git(f.repo, 'tag', 'v99.0.0');
  const info = capture(f.repo);
  assert.equal(info.source.commit, f.source); assert.equal(info.source.repository, 'MoBluey/home-plus');
  assert.equal(info.upstream.version, null); assert.equal(info.upstream.commit, null); assert.equal(info.source.base, null); assert.equal(info.customised, true);
});

test('desktop environment and local tags cannot impersonate an official CI release', t => {
  const f = fixture(t), { capture } = require('../tools/update-provenance');
  git(f.repo, 'checkout', '-q', '-b', 'desktop-source', f.source);
  git(f.repo, 'remote', 'set-url', 'origin', 'https://github.com/pbalazaar/home-plus.git');
  git(f.repo, 'config', 'branch.desktop-source.remote', 'origin'); git(f.repo, 'tag', 'v99.0.0');
  const names = ['GITHUB_ACTIONS', 'GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_REF'], old = names.map(name => process.env[name]);
  try {
    process.env.GITHUB_ACTIONS = 'false'; process.env.GITHUB_REPOSITORY = 'pbalazaar/home-plus'; process.env.GITHUB_SHA = f.source; process.env.GITHUB_REF = 'refs/tags/v99.0.0';
    const info = capture(f.repo); assert.equal(info.source.commit, f.source); assert.equal(info.upstream.commit, null); assert.equal(info.source.base, null); assert.equal(info.customised, true);
  } finally { names.forEach((name, i) => { if (old[i] === undefined) delete process.env[name]; else process.env[name] = old[i]; }); }
});

test('preparation rejects a claimed fork that is not the source checkout identity', t => {
  const f = fixture(t);
  assert.throws(() => prepare({ ...f.options, sourceRepository: 'Other/fork' }), /repository|identity/i);
  assert.equal(fs.existsSync(f.options.destination), false);
  assert.throws(() => prepare({ ...f.options, upstreamRepository: 'Other/upstream' }), /repository|identity/i);
});

test('two verified source forks retain schema 1 provenance and reject cross-fork replay', t => {
  const { capture } = require('../tools/update-provenance'), { sameCandidate } = require('../tools/prepare-fork-update');
  for (const repository of ['MoBluey/home-plus', 'Example-Owner/custom.home-plus']) {
    const f = fixture(t);
    git(f.repo, 'remote', 'set-url', 'origin', 'git@github.com:' + repository + '.git');
    const result = prepare({ ...f.options, sourceRepository: repository });
    assert.equal(result.schemaVersion, 1); assert.equal(result.source.repository, repository);
    const info = JSON.parse(fs.readFileSync(path.join(f.options.destination, 'update-info-build.json')));
    assert.equal(info.source.repository, repository); assert.equal(info.schemaVersion, 1);
    git(f.repo, 'checkout', '-q', 'source-identity');
    assert.equal(capture(f.repo).source.repository, repository);
    assert.equal(sameCandidate(result, f.source, f.upstream, repository), true);
    assert.equal(sameCandidate(result, f.source, f.upstream, 'Other/fork'), false);
  }
});

test('trusted publisher checks checkout and result identity before writes for either fork', t => {
  const { publish } = require('../tools/publish-fork-update');
  const ID = 'com.github.pbalazaar.homeplus', crypto = require('node:crypto');
  for (const repository of ['MoBluey/home-plus', 'Example-Owner/custom.home-plus']) {
    const f = fixture(t);
    git(f.repo, 'remote', 'set-url', 'origin', 'https://github.com/' + repository + '.git');
    const result = prepare({ ...f.options, sourceRepository: repository });
    const dist = path.join(f.options.destination, 'dist'); fs.mkdirSync(dist);
    const data = Buffer.from('checked custom payload'), sha = crypto.createHash('sha256').update(data).digest('hex'), name = ID + '_1.1.0_all.ipk';
    fs.writeFileSync(path.join(dist, name), data);
    fs.writeFileSync(path.join(dist, ID + '.manifest.json'), JSON.stringify({ id: ID, version: '1.1.0', ipkUrl: name, ipkSize: data.length, ipkHash: { sha256: sha } }));
    const ready = finish(result, f.options.destination, true), file = path.join(f.dir, ID + '.update.json');
    fs.writeFileSync(file, JSON.stringify(ready)); fs.writeFileSync(path.join(f.dir, path.basename(ready.artifact.url)), data);
    git(f.repo, 'checkout', '-q', 'source-identity');
    // Stub only network CLI calls. Production publication still reads the real
    // result/package and verifies the Git checkout, on every supported host.
    const calls = [], exec = cp.execFileSync;
    const env = { GITHUB_ACTIONS: 'true', GITHUB_REPOSITORY: repository, GH_REPO: repository, GITHUB_SHA: f.source };
    const expected = { repository, source: f.source, version: '1.1.0' };
    const run = changes => {
      Object.assign(process.env, env, changes);
      return publish(f.repo, file, { ...expected, repository: process.env.GITHUB_REPOSITORY });
    };
    try {
      cp.execFileSync = function (command, args) {
        if (command !== 'gh') return exec.apply(this, arguments);
        calls.push(args); return args[1] === 'view' ? JSON.stringify({ assets: [] }) : '';
      };
      run({});
      assert.equal(calls.length, 3);
      for (const args of calls) assert.equal(args[args.indexOf('--repo') + 1], repository);
      assert.equal(calls[1].includes('--clobber'), false); assert.equal(calls[2].includes('--clobber'), true);
      const before = calls.length;
      for (const change of [{ GH_REPO: 'Other/fork' }, { GITHUB_REPOSITORY: 'Other/fork', GH_REPO: 'Other/fork' }, { GITHUB_SHA: 'f'.repeat(40) }, { GITHUB_ACTIONS: 'false' }]) assert.throws(() => run(change));
      for (const change of [{ source: { ...ready.source, repository: 'Other/fork' } }, { artifact: { ...ready.artifact, url: ready.artifact.url.replace(repository, 'Other/fork') } }]) {
        fs.writeFileSync(file, JSON.stringify({ ...ready, ...change })); assert.throws(() => run({}));
      }
      assert.equal(calls.length, before);
    } finally { cp.execFileSync = exec; ciNames.forEach(name => { delete process.env[name]; }); }
  }
});

test('fork workflow keeps candidate code in read-only jobs and publication in trusted checkout', () => {
  const workflow = fs.readFileSync(path.resolve(__dirname, '../.github/workflows/fork-updates.yml'), 'utf8');
  assert.match(workflow, /if: github.repository != 'pbalazaar\/home-plus' && github.event.repository.fork == true/);
  const result = workflow.slice(workflow.indexOf('\n  result:'));
  const candidates = workflow.slice(0, workflow.indexOf('\n  result:'));
  assert.match(candidates, /permissions:\n  contents: read/); assert.doesNotMatch(candidates, /contents: write/);
  assert.match(result, /contents: write/); assert.match(result, /ref: \$\{\{ github.sha \}\}/);
  assert.doesNotMatch(result, /name: fork-candidate|candidate\.tar|tools\/build-release/);
  assert.match(result, /persist-credentials: false/);
});
