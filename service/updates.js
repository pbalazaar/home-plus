/* Confirmed updates: checking never downloads a package or changes installation. */
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const release = require('./update-release');
const { RUNTIME_COOKIE } = require('./update-archive');
const { compareVersions, validateArtifact, UPSTREAM, repository, forkRepository, plain } = release;
const DAY = 24 * 60 * 60 * 1000;
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
function provenance(appDir, serviceDir) {
  try {
    const p = JSON.parse(fs.readFileSync(path.join(appDir, 'update-info.json'), 'utf8'));
    if (p.schemaVersion !== 1 || !p.upstream || p.upstream.repository !== UPSTREAM || !p.source || !repository(p.source.repository) ||
        !/^[0-9a-f]{40}$/.test(p.source.commit) || !/^[0-9a-f]{40}$/.test(p.upstream.commit) || !p.fileHashes || !Object.keys(p.fileHashes).length || typeof p.customised !== 'boolean' || !Array.isArray(p.customisations)) throw new Error('Unknown build provenance');
    release.version(p.upstream.version);
    const seen = {};
    function walk(dir, prefix) {
      for (const name of fs.readdirSync(dir)) {
        if (name === 'update-info.json' || name === 'api-token' || name === 'cache' || name === 'data') continue;
        const file = path.join(dir, name), key = prefix + '/' + name, st = fs.lstatSync(file);
        if (key === 'application/' + RUNTIME_COOKIE) {
          if (!st.isFile()) throw new Error('Unverified runtime file');
          continue;
        }
        if (prefix === 'application' && /^qml-[a-z0-9]+$/.test(name) && st.isSymbolicLink() && fs.readlinkSync(file) === 'qml') continue;
        if (st.isDirectory()) walk(file, key);
        else if (!st.isFile() || p.fileHashes[key] !== hash(fs.readFileSync(file))) throw new Error('Installed files have unverified changes');
        else seen[key] = true;
      }
    }
    walk(appDir, 'application'); walk(serviceDir, 'service');
    if (Object.keys(p.fileHashes).some(k => k !== 'application/' + RUNTIME_COOKIE && !seen[k])) throw new Error('Installed file inventory is incomplete');
    return { known: true, info: p, label: p.customised ? 'Custom build' : 'Official build' };
  } catch (e) { return { known: false, label: 'Unknown build', warning: 'This installation has unknown or unverified changes. Replacing it will discard those changes.' }; }
}
// Called only by the live service, synchronously with its other config writers.
// The detached installer must never replace config.json, even during recovery.
function restoreConfig(state, writeConfig, suspendDefault) {
  const saved = JSON.parse(fs.readFileSync(state.configBackup, 'utf8'));
  if (!saved || typeof saved !== 'object' || Array.isArray(saved)) throw new Error('Recovery config snapshot is unusable');
  let cfg;
  try { cfg = JSON.parse(fs.readFileSync(state.configPath, 'utf8')); }
  catch (e) { if (e.code !== 'ENOENT' && !(e instanceof SyntaxError)) throw e; }
  if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) cfg = saved;
  for (const key of ['homeMode', 'homeOverride']) {
    if (Object.prototype.hasOwnProperty.call(saved, key)) cfg[key] = saved[key]; else delete cfg[key];
  }
  if (suspendDefault && state.defaultHome) { cfg.defaultHomeEnabled = false; cfg.homeOverride = false; }
  writeConfig(cfg);
}
function createUpdates(options) {
  const now = options.now || Date.now;
  const fetchRelease = options.fetchRelease || release.fetchRelease;
  let current = provenance(options.appDir, options.serviceDir || __dirname), candidate = null, checking = false, installing = false, timer = null, closed = false;
  let feed = current.known && forkRepository(current.info.source.repository) ? current.info.source.repository : null;
  let view = { returnValue: true, currentVersion: options.version, upstreamVersion: '', phase: 'idle', currentSource: current.label,
    automatic: true, prompt: false, ignoredVersion: '', confirmation: '', title: 'Home+ Updates', summary: '', notes: '', warning: current.warning || '', message: '', canInstallPreserved: false, canInstallUpstream: false, progress: '' };
  function refreshProvenance() {
    current = provenance(options.appDir, options.serviceDir || __dirname);
    feed = current.known && forkRepository(current.info.source.repository) ? current.info.source.repository : null;
    view.currentSource = current.label;
    view.warning = current.warning || '';
  }
  function prefs() { return options.readConfig().updates || {}; }
  function save(p) { const cfg = options.readConfig(); cfg.updates = Object.assign({}, cfg.updates, p); options.writeConfig(cfg); }
  function transaction() {
    try {
      const t = JSON.parse(fs.readFileSync(path.join(options.stateDir, 'transaction.json'), 'utf8'));
      if (t.schemaVersion !== 1 || typeof t.active !== 'boolean') throw new Error('Invalid recovery state');
      return t;
    } catch (e) { return e.code === 'ENOENT' ? null : { active: true, phase: 'recovering', message: 'Update recovery state is unreadable. The previous backup has been retained.' }; }
  }
  function busy() { const t = transaction(); return installing || !!(t && t.active); }
  function state() {
    const p = prefs(), t = transaction();
    const result = Object.assign({}, view, { automatic: p.automatic !== false, ignoredVersion: p.ignoredVersion || '' });
    if (t && (t.active || view.phase === 'installing' || !candidate)) { result.phase = t.phase === 'uncertain' ? 'recovering' : ['prepared', 'verifying', 'awaiting-entry', 'restoring-home'].includes(t.phase) ? 'installing' : t.phase; result.progress = t.message || ''; result.message = t.message || ''; }
    result.prompt = !!candidate && !busy() && p.automatic !== false && (!p.ignoredVersion || compareVersions(candidate.upstream.version, p.ignoredVersion) > 0);
    if (busy()) { result.canInstallPreserved = false; result.canInstallUpstream = false; }
    return result;
  }
  function normalise(data) {
    const u = data && data.upstream;
    if (!u || u.repository !== UPSTREAM || u.tag !== 'v' + u.version || !/^[0-9a-f]{40}$/.test(u.commit)) throw new Error('Invalid upstream release');
    validateArtifact(u.artifact, UPSTREAM, u.tag);
    if (u.artifact.version !== u.version) throw new Error('Upstream version mismatch');
    const r = feed ? data.result : null;
    if (r && (r.schemaVersion !== 1 || !r.upstream || r.upstream.repository !== UPSTREAM || r.upstream.version !== u.version || r.upstream.tag !== u.tag || r.upstream.commit !== u.commit ||
        !r.source || r.source.repository !== feed || !/^[0-9a-f]{40}$/.test(r.source.commit) || !/^[0-9a-f]{40}$/.test(r.source.base) || !['ready', 'conflict', 'failed'].includes(r.status) || !Array.isArray(r.customisations) || !Array.isArray(r.conflicts))) throw new Error('Custom build result does not match the upstream release');
    let preserved = current.known && current.info.source.repository === UPSTREAM && !current.info.customised ? u.artifact : null;
    if (r && r.status === 'ready') {
      if (r.artifact) { if (r.artifact.version !== u.version) throw new Error('Custom package version mismatch'); validateArtifact(r.artifact, feed, 'upstream-v' + u.version, r.source.commit); preserved = r.artifact; }
      else if (!r.customisations.length) preserved = u.artifact;
      else throw new Error('Custom build package is missing');
    }
    // Unknown installed edits cannot be represented by a remotely prepared custom build.
    if (!current.known) preserved = null;
    return { upstream: u, result: r || null, preserved, resultMessage: plain(data.resultMessage) };
  }
  function show(c) {
    candidate = c;
    const r = c.result;
    view = Object.assign({}, view, { upstreamVersion: c.upstream.version, phase: c.preserved ? 'ready' : r && r.status !== 'ready' ? r.status : 'ready',
      title: r && r.status !== 'ready' ? 'Custom build could not be prepared' : 'Home+ update available',
      summary: c.preserved === c.upstream.artifact ? 'Official Home+ ' + c.upstream.version : c.preserved ? 'Custom changes are preserved. Upstream ' + c.upstream.version : 'Official upstream ' + c.upstream.version + ' is available.',
      notes: plain(c.upstream.notes), message: r ? plain(r.message) : c.resultMessage,
      warning: (current.warning ? current.warning + ' ' : '') + 'Install upstream drops custom code.',
      canInstallPreserved: !!c.preserved, canInstallUpstream: true, progress: '',
      confirmation: hash(JSON.stringify({ installed: current, upstream: c.upstream, preserved: c.preserved, result: c.result })) });
  }
  async function check() {
    if (checking || busy()) return;
    checking = true; view.phase = 'checking'; view.message = 'Checking releases';
    try {
      refreshProvenance();
      const c = normalise(await fetchRelease(feed));
      if (compareVersions(c.upstream.version, options.version) <= 0) { candidate = null; view.phase = 'idle'; view.message = 'Home+ is up to date'; view.upstreamVersion = c.upstream.version; view.canInstallPreserved = false; view.canInstallUpstream = false; view.confirmation = ''; }
      else show(c);
      save({ lastCheck: now(), nextCheck: now() + DAY, cached: c });
    } catch (e) { candidate = null; view.phase = 'error'; view.message = plain(e.message); view.confirmation = ''; view.canInstallPreserved = false; view.canInstallUpstream = false; save({ nextCheck: now() + 60 * 60 * 1000 }); }
    checking = false;
  }
  async function install(body) {
    const old = view.confirmation;
    installing = true; view.phase = 'installing'; view.progress = 'Verifying the confirmed release';
    try {
      if (options.externalBusy && options.externalBusy()) throw new Error('Another Home+ setup operation is running');
      if (!options.isRoot()) throw new Error('Homebrew Channel root access is needed');
      if (!options.foreground()) throw new Error('Update cancelled because Home+ is no longer in front');
      refreshProvenance();
      const fresh = normalise(await fetchRelease(feed)); show(fresh);
      if (view.confirmation !== old) throw new Error('Release changed. Check and confirm the new package.');
      const artifact = body.target === 'preserve' ? fresh.preserved : fresh.upstream.artifact;
      if (!artifact) throw new Error('The confirmed package is no longer available');
      view.phase = 'installing'; view.progress = 'Preparing a verified backup. Installation continues independently.';
      const launch = options.launchWorker || require('./update-worker').prepare;
      await launch(Object.assign({}, options, { artifact, confirmation: old, target: body.target }));
    } catch (e) { view.phase = 'error'; view.message = plain(e.message); view.progress = ''; }
    installing = false;
  }
  try { const cached = prefs().cached; if (cached && compareVersions(cached.upstream.version, options.version) > 0) show(normalise(cached)); } catch (e) { /* an older or invalid cache is checked again */ }
  async function act(body) {
    if (!body || typeof body !== 'object') throw new Error('Invalid update action');
    if (body.action === 'automatic') { if (typeof body.enabled !== 'boolean') throw new Error('Invalid automatic preference'); save({ automatic: body.enabled }); return state(); }
    if (body.action === 'check') { check(); return state(); }
    if (body.action === 'keep') {
      if (!candidate || body.version !== candidate.upstream.version) throw new Error('The current release has changed');
      const ignored = prefs().ignoredVersion;
      if (!ignored || compareVersions(body.version, ignored) > 0) save({ ignoredVersion: body.version });
      return state();
    }
    if (body.action === 'install') {
      if (busy() || checking) throw new Error('An update transaction is already running');
      if (!candidate || body.version !== candidate.upstream.version || body.confirmation !== view.confirmation || !['preserve', 'upstream'].includes(body.target) || (body.target === 'preserve' && !candidate.preserved)) throw new Error('The exact current confirmation and target are required');
      install(body); return state();
    }
    throw new Error('Unknown update action');
  }
  function start() {
    if (timer || closed) return;
    const tick = () => { timer = null; if (closed) return; if (prefs().automatic !== false && now() >= (prefs().nextCheck || 0)) check(); timer = setTimeout(tick, 60 * 60 * 1000); if (timer.unref) timer.unref(); };
    timer = setTimeout(tick, options.startupDelay === undefined ? 30000 : options.startupDelay); if (timer.unref) timer.unref();
  }
  return { state, act, start, busy, close() { closed = true; clearTimeout(timer); timer = null; } };
}
module.exports = { restoreConfig, createUpdates, compareVersions, validateArtifact, provenance };
