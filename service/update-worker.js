/* Copied outside the package before install. Never depends on the launching service surviving. */
'use strict';
const fs = require('fs');
const path = require('path');
const { execFile, spawn } = require('child_process');
const archive = require('./update-archive');
const release = require('./update-release');
const recovery = require('./default-home-guard');
const { requestJSON } = require('./net');
const { mkdirSync } = require('./mkdir');
const APP = release.APP, SERVICE = APP + '.service';
const HBC = 'luna://org.webosbrew.hbchannel.service/';
const MANAGER = 'luna://com.webos.applicationManager/';
const ELEVATE = '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service';
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const quote = s => "'" + s.replace(/'/g, "'\\''") + "'";
const read = file => JSON.parse(fs.readFileSync(file, 'utf8'));
function privateDir(dir) {
  mkdirSync(dir, 0o700);
  const st = fs.lstatSync(dir);
  if (!st.isDirectory() || st.isSymbolicLink() || (process.getuid && st.uid !== process.getuid())) throw new Error('Unsafe recovery directory');
  fs.chmodSync(dir, 0o700);
}
function runFile(command, args, onResult) {
  return new Promise((resolve, reject) => execFile(command, args, { timeout: 15000, maxBuffer: 1024 * 1024 }, (err, stdout, stderr) => {
    if (onResult) { try { onResult(recovery.commandDiagnostic(err, stdout, stderr)); } catch (ignored) { /* diagnostics cannot change installation */ } }
    if (err) reject(new Error('Required update tool failed: ' + path.basename(command))); else resolve(String(stdout));
  }));
}
// Only these generated public activation files, never paths read from a manifest.
function activationDiagnostic() {
  const files = [];
  for (const base of ['/var/luna-service2-dev', '/var/luna-service2']) {
    for (const name of ['/services.d/' + SERVICE + '.service', '/manifests.d/' + APP + '.json']) {
      const file = base + name, record = { file }; let fd;
      try {
        const before = fs.lstatSync(file);
        if (!before.isFile() || fs.realpathSync(file) !== file) throw new Error('not a regular canonical file');
        fd = fs.openSync(file, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0) | (fs.constants.O_NONBLOCK || 0));
        const st = fs.fstatSync(fd);
        if (!st.isFile() || st.dev !== before.dev || st.ino !== before.ino) throw new Error('activation file changed while opening');
        const data = Buffer.alloc(Math.min(st.size, 16384)), bytes = fs.readSync(fd, data, 0, data.length, 0);
        Object.assign(record, { mode: st.mode, uid: st.uid, gid: st.gid, size: st.size, contents: data.slice(0, bytes).toString(), truncated: st.size > data.length });
      } catch (e) { record.error = e.code || e.message; }
      finally { if (fd !== undefined) fs.closeSync(fd); }
      files.push(record);
    }
  }
  return { files };
}
function startupDiagnostic(file, state, kind, data) {
  // Write once per phase: recovery must never overwrite the failed target evidence.
  // This is private evidence, not transaction/API state, and failure is non-fatal.
  let fd;
  try {
    if (!file || !/^[0-9a-f]{24}$/.test(state.id)) return;
    const dir = path.dirname(file), st = fs.lstatSync(dir);
    if (!st.isDirectory() || st.isSymbolicLink() || (process.getuid && st.uid !== process.getuid()) || (process.platform !== 'win32' && (st.mode & 0o077))) return;
    const phase = state.installer === 'backup' ? 'backup' : 'target';
    fd = fs.openSync(path.join(dir, state.id + '-' + phase + '-' + kind + '.json'), 'wx', 0o600);
    fs.writeFileSync(fd, JSON.stringify(Object.assign({ at: Date.now() }, typeof data === 'function' ? data() : data)));
    fs.fsyncSync(fd); recovery.syncDir(dir);
  } catch (ignored) { /* preserve existing evidence, never change update/recovery outcome */ }
  finally { if (fd !== undefined) { try { fs.closeSync(fd); } catch (ignored) { /* diagnostic only */ } } }
}
// webos-service names its process with exactly the service ID. Match that entire
// cmdline and the actual Node executable, never a substring in a shell/helper.
function serviceProcess(pid, node) {
  if (pid === process.pid) return null;
  try {
    const args = fs.readFileSync('/proc/' + pid + '/cmdline', 'utf8').split('\0').filter(Boolean);
    if (args.length !== 1 || args[0] !== SERVICE || fs.realpathSync('/proc/' + pid + '/exe') !== node) return null;
    const start = processStart(pid);
    if (!start) throw new Error('Cannot identify the running Home+ service');
    return { pid, start };
  } catch (e) { if (e.code === 'ENOENT' || e.code === 'ESRCH') return null; throw e; }
}
async function stopService() {
  const node = fs.realpathSync(process.execPath);
  const owners = fs.readdirSync('/proc').filter(name => /^\d+$/.test(name)).map(name => serviceProcess(Number(name), node)).filter(Boolean);
  const same = owner => { const current = serviceProcess(owner.pid, node); return current && current.start === owner.start; };
  for (const owner of owners) {
    if (!same(owner)) continue;
    try { process.kill(owner.pid, 'SIGTERM'); } catch (e) { if (e.code !== 'ESRCH') throw e; }
  }
  const end = Date.now() + 3000;
  while (owners.some(same)) {
    if (Date.now() >= end) throw new Error('Home+ service did not stop after elevation');
    await wait(100);
  }
}
function finalReply(r, local) {
  if (!r || typeof r !== 'object' || Array.isArray(r)) return false;
  if (r.returnValue === false || (r.details && r.details.errorCode !== undefined)) throw new Error('Package installer refused the update');
  return local ? r.statusValue === 30 && r.details && r.details.packageId === APP : r.finished === true;
}
function installSubscription(uri, params, local, options) {
  return new Promise((resolve, reject) => {
    const command = 'exec luna-send -i ' + quote(uri) + ' ' + quote(JSON.stringify(params));
    const child = spawn('script', ['-q', '-c', command, '/dev/null'], { detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let partial = '', bytes = 0, done = false;
    const uncertain = message => { const error = new Error(message); error.installerUncertain = true; return error; };
    const finish = error => {
      if (done) return; done = true; clearTimeout(timer);
      try { process.kill(-child.pid, 'SIGTERM'); } catch (e) { child.kill(); }
      if (error) reject(error); else resolve();
    };
    const timer = setTimeout(() => finish(uncertain('Installer did not report final completion')), (options && options.timeout) || 180000);
    child.stdout.on('data', data => {
      bytes += data.length; if (bytes > 1024 * 1024) return finish(uncertain('Installer output exceeds limit'));
      partial += data.toString();
      let pos;
      while ((pos = partial.indexOf('\n')) >= 0) {
        const line = partial.slice(0, pos).trim(); partial = partial.slice(pos + 1);
        let r; try { r = JSON.parse(line); } catch (e) { continue; }
        try { if (finalReply(r, local)) finish(); } catch (e) { finish(e); }
      }
    });
    child.stderr.on('data', () => {});
    child.on('error', () => finish(new Error('Installer terminal could not start')));
    child.on('exit', () => { if (!done) finish(uncertain('Installer exited without final completion')); });
  });
}
function production(state, file) {
  const luna = recovery.luna;
  const diagnose = (kind, data) => startupDiagnostic(file, state, kind, data);
  const accepted = (uri, params) => recovery.accepted(luna, uri, params);
  const front = async () => { const r = await accepted(MANAGER + 'getForegroundAppInfo', {}); return r.appId || (r.foregroundAppInfo && r.foregroundAppInfo[0] && r.foregroundAppInfo[0].appId) || ''; };
  return {
    foreground: front,
    installTarget: () => installSubscription(HBC + 'install', { id: APP, ipkUrl: state.artifact.url, ipkHash: state.artifact.sha256, subscribe: true }, false),
    installBackup: () => installSubscription('luna://com.webos.appInstallService/dev/install', { id: APP, ipkUrl: state.backup, subscribe: true }, true),
    async elevate() {
      try { await runFile(ELEVATE, [SERVICE], result => diagnose('elevation', result)); }
      finally { diagnose('activation', activationDiagnostic); }
      // Like tools/install.sh: role rescanning does not restart a running service.
      // Called only after final installer completion and payload verification,
      // for both target and rollback; the independent helper stays alive.
      await stopService();
    },
    async health(version) {
      const end = Date.now() + 20000; let last = 'no service response';
      while (Date.now() < end) {
        try {
          await recovery.accepted((uri, params) => luna(uri, params, result => diagnose('ping', result)), 'luna://' + SERVICE + '/ping', {});
          const r = await requestJSON('GET', 'http://127.0.0.1:47823/api/status', { timeout: 2000 });
          last = 'HTTP ' + r.status + ', version ' + release.plain(r.json && r.json.version, 40) + ', elevated ' + !!(r.json && r.json.elevated === true);
          if (r.status === 200 && r.json && r.json.id === SERVICE && r.json.version === version) {
            if (r.json.elevated === true) return;
            await requestJSON('POST', 'http://127.0.0.1:47823/api/restart', { timeout: 2000 });
          }
        } catch (e) { last = release.plain(e.message, 160); }
        await wait(500);
      }
      throw new Error('Updated service health check failed (expected ' + version + '; ' + last + ')');
    },
    async enter() {
      const id = await front();
      // A foreground app selected during installation owns the screen, even LG Home.
      if (id && id !== APP) return false;
      await accepted(MANAGER + 'launch', { id: APP });
      const end = Date.now() + 15000; let stable = 0;
      while (Date.now() < end) { if (await front() !== APP) throw new Error('Home+ did not stay open'); if (++stable >= 10) return true; await wait(500); }
      throw new Error('Home+ did not stay open');
    },
    async restoreHome() {
      const saved = read(state.configBackup);
      let suspendDefault = false, preserveDefault = false;
      const defaults = state.defaultHome ? await recovery.readDefaults(luna) : {};
      if (state.defaultHome && defaults.home !== APP) {
        if (!state.homeStateFile || !fs.existsSync(state.homeStateFile)) throw new Error('Original Home recovery state is unavailable');
        const home = recovery.load(state.homeStateFile);
        preserveDefault = !!(defaults.home && defaults.home !== home.stockHome && defaults.home !== home.before.home);
        suspendDefault = true;
        if (!preserveDefault && await front() !== APP) throw new Error('Home restoration waits for Home+ to be in front');
      }
      const token = fs.readFileSync(path.join(state.directories[0], 'api-token'), 'utf8').trim();
      const post = body => requestJSON('POST', 'http://127.0.0.1:47823/api/home', { timeout: 45000, headers: { 'X-Homeplus-Token': token }, body });
      const acceptedHome = r => r.status === 200 && r.json && r.json.returnValue !== false;
      const status = await requestJSON('GET', 'http://127.0.0.1:47823/api/status', { timeout: 2000 });
      if (status.status !== 200 || !status.json || status.json.id !== SERVICE ||
          ![state.previousVersion, state.artifact.version].includes(status.json.version)) throw new Error('Home restoration service is unavailable');
      let r;
      if (status.json.updateRecovery === 1) {
        // The service owns the complete synchronous read/modify/write, alongside
        // account and layout saves. No private settings cross the HTTP boundary.
        r = await post({ updateTransaction: state.id, updateConfig: suspendDefault ? 'suspend' : 'restore', defaultHome: state.defaultHome && !preserveDefault });
        if (!acceptedHome(r) || r.json.updateRecovery !== 1) throw new Error('Home configuration restoration was not acknowledged');
      } else {
        // Official 0.25 has no updater route. Use its existing Home API, never
        // interpret ignored updater fields as success or write behind its back.
        const cfg = read(state.configPath);
        if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) throw new Error('Home configuration requires recovery by the previous service');
        if (suspendDefault) {
          r = await post({ defaultHome: false });
          if (!acceptedHome(r) || r.json.defaultHomeEnabled !== false) throw new Error('Previous Home settings could not be reconciled');
        }
        const mode = preserveDefault || saved.homeOverride === false ? 'off' : saved.homeMode || 'watch';
        const defaultHome = state.defaultHome && !preserveDefault;
        r = await post(defaultHome ? { defaultHome: true } : { mode });
        if (!acceptedHome(r) || (defaultHome ? r.json.defaultHomeEnabled !== true : r.json.mode !== mode)) throw new Error('Home configuration restoration was not acknowledged');
      }
      if (state.defaultHome && !preserveDefault && (await recovery.readDefaults(luna)).home !== APP) throw new Error('Default Home compatibility restoration failed');
    },
    async fallback() {
      // Platform recovery is independent of the service. Its config is left to
      // that service: being unreachable does not prove its writers have stopped.
      if (!state.homeStateFile || !fs.existsSync(state.homeStateFile)) return;
      const s = recovery.load(state.homeStateFile);
      await recovery.restore(luna, s.before, s.stockHome);
      s.active = false; recovery.atomic(state.homeStateFile, s);
    }
  };
}
function ownHook(s) {
  try { return fs.readlinkSync(s.hook) === s.boot; } catch (e) { if (e.code === 'ENOENT') return false; throw e; }
}
function finishHook(s) {
  if (ownHook(s)) { fs.unlinkSync(s.hook); recovery.syncDir(path.dirname(s.hook)); }
}
function load(file) {
  const s = read(file);
  if (s.schemaVersion !== 1 || typeof s.active !== 'boolean' || !/^[0-9a-f]{24}$/.test(s.id) || !Array.isArray(s.directories) || s.directories.length !== 3 ||
      ![s.backup, s.targetFile, s.configPath, s.configBackup, s.hook, s.boot].concat(s.directories).every(p => typeof p === 'string' && path.isAbsolute(p)) || !s.artifact) throw new Error('Invalid update recovery record');
  release.version(s.previousVersion); release.version(s.artifact.version);
  return s;
}
const generationId = value => typeof value === 'string' && /^runtime-v2:[0-9a-f]{64}$/.test(value);
function bootId(runtime, mountInfo) {
  runtime = runtime || '/dev'; // fixed volatile parent; never fall back to another mount
  if (mountInfo === undefined) mountInfo = fs.readFileSync('/proc/self/mountinfo', 'utf8');
  const root = fs.lstatSync(runtime);
  const mounted = mountInfo.split('\n').some(line => {
    const parts = line.split(' - '), fields = parts[0].split(' ');
    return parts.length === 2 && fields[3] === '/' && fields[4] === runtime && ['tmpfs', 'devtmpfs'].includes(parts[1].split(' ')[0]);
  });
  if (!mounted || !root.isDirectory() || root.isSymbolicLink() || root.uid !== 0 || (root.mode & 0o022) || fs.realpathSync(runtime) !== runtime)
    throw new Error('Cannot verify volatile runtime mount for update recovery');
  const dir = path.join(runtime, 'homeplus-update-boot'), file = path.join(dir, 'generation.json');
  let created = false;
  try { fs.mkdirSync(dir, 0o700); created = true; } catch (e) { if (e.code !== 'EEXIST') throw e; }
  const st = fs.lstatSync(dir);
  if (!st.isDirectory() || st.isSymbolicLink() || st.uid !== 0 || (st.mode & 0o777) !== 0o700)
    throw new Error('Unsafe update boot generation directory');
  // Only the exclusive mkdir winner may initialise. A killed creator leaves an
  // incomplete directory that must fail closed until the volatile mount resets.
  // No updater path removes this directory or replaces an existing generation.
  if (created) recovery.atomic(file, { schemaVersion: 1, generation: require('crypto').randomBytes(32).toString('hex') });
  const marker = fs.lstatSync(file);
  if (!marker.isFile() || marker.uid !== 0 || (marker.mode & 0o777) !== 0o600 || marker.size > 256)
    throw new Error('Unsafe update boot generation record');
  const record = read(file), id = 'runtime-v2:' + record.generation;
  if (record.schemaVersion !== 1 || !generationId(id)) throw new Error('Invalid update boot generation record');
  return id;
}
function processStart(pid) {
  try { const stat = fs.readFileSync('/proc/' + pid + '/stat', 'utf8'); return stat.slice(stat.lastIndexOf(')') + 2).split(' ')[19]; }
  catch (e) { return ''; }
}
function workerAlive(owner, boot) {
  // A legacy raw UUID or runtime-v1 token cannot prove a different boot during migration.
  if (generationId(owner.boot) && owner.boot !== boot) return false;
  try { process.kill(owner.pid, 0); }
  catch (e) { return e.code !== 'ESRCH'; }
  const start = processStart(owner.pid);
  return !owner.start || !start || start === owner.start;
}
function lockRecord(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const owner = JSON.parse(fs.readFileSync(fd, 'utf8'));
    if (!owner || !Number.isInteger(owner.pid) || owner.pid <= 0 || typeof owner.boot !== 'string') throw new Error('Invalid worker owner');
    return { owner, stat: fs.fstatSync(fd) };
  } finally { fs.closeSync(fd); }
}
function sameLock(file, stat) {
  try { const now = fs.statSync(file); return now.dev === stat.dev && now.ino === stat.ino; }
  catch (e) { if (e.code === 'ENOENT') return false; throw e; }
}
const successor = (lock, owner) => lock + '.after-' + archive.hash(Buffer.from(JSON.stringify(owner)));
function acquireWorker(file, boot) {
  const lock = file + '.worker', candidate = lock + '.owner-' + require('crypto').randomBytes(12).toString('hex');
  // Write and flush privately, then publish exclusively with link(2). A torn private file is never a lock.
  recovery.atomic(candidate, { pid: process.pid, boot: boot, start: processStart(process.pid), token: path.basename(candidate) });
  let at = lock, anchor = null, chain = [];
  try {
    for (;;) {
      let linked = false;
      try { fs.linkSync(candidate, at); linked = true; }
      catch (e) { if (e.code !== 'EEXIST') throw e; }
      if (linked) {
        recovery.syncDir(path.dirname(lock));
        if (!anchor) anchor = fs.statSync(at);
        if (!sameLock(lock, anchor)) { fs.unlinkSync(at); at = lock; anchor = null; chain = []; continue; }
        chain.push(at);
        return () => {
          if (!sameLock(lock, anchor)) return;
          fs.unlinkSync(lock);
          for (const entry of chain) if (entry !== lock) fs.unlinkSync(entry);
          recovery.syncDir(path.dirname(lock));
        };
      }
      let previous;
      try { previous = lockRecord(at); }
      catch (e) { if (e.code !== 'ENOENT') throw e; at = lock; anchor = null; chain = []; continue; }
      if (!anchor) anchor = previous.stat;
      if (!sameLock(lock, anchor)) { at = lock; anchor = null; chain = []; continue; }
      if (workerAlive(previous.owner, boot)) throw new Error('Update recovery is already running');
      // Never unlink a stale lock: concurrent reapers could otherwise unlink the new live owner.
      // Exactly one successor wins each immutable owner's exclusive link; interrupted takeover is recoverable.
      chain.push(at); at = successor(lock, previous.owner);
    }
  } finally { fs.unlinkSync(candidate); }
}
async function run(file, injected) {
  let s = load(file);
  if (!s.active) { finishHook(s); return; }
  const boot = injected && injected.bootId ? injected.bootId() : bootId();
  if (!generationId(boot)) throw new Error('Invalid current boot generation');
  const releaseWorker = acquireWorker(file, boot);
  try { s = load(file); } catch (e) { releaseWorker(); throw e; }
  if (!s.active) { releaseWorker(); finishHook(s); return; }
  const ops = injected || production(s, file);
  let replacing = s.phase !== 'prepared', stage = 'resume';
  const failure = (key, error) => { s[key] = { stage, message: release.plain(error.message, 300), at: Date.now() }; recovery.atomic(file, s); };
  const save = (phase, message) => { s.phase = phase; s.message = message; recovery.atomic(file, s); };
  async function install(kind) {
    s.installBoot = boot; s.installerPending = true; s.installer = kind;
    save(kind === 'target' ? 'installing' : 'recovering', kind === 'target' ?
      'Installing Home+. Recovery continues independently.' : 'Reinstalling the verified previous package.');
    stage = 'install-' + kind;
    try { await (kind === 'target' ? ops.installTarget() : ops.installBackup()); }
    catch (e) {
      if (!e.installerUncertain) {
        s.installerPending = false;
        save('recovering', 'Installer reported failure. Preparing recovery.');
      }
      throw e;
    }
    s.installerPending = false;
    save('verifying', 'Checking the installed package and service');
  }
  async function cancel(error) {
    s.cancelled = true;
    save('restoring-home', 'Restoring Home settings before cancelling the update');
    stage = 'restore-home';
    try { await ops.restoreHome(); }
    catch (e) {
      failure('recoveryFailure', e);
      try { await ops.fallback(); } catch (ignored) { /* retain independent recovery */ }
      save('restoring-home', 'Update cancelled. Home settings need recovery; the backup and recovery hook are retained.');
      return;
    }
    s.active = false; save('error', 'Update stopped before replacement: ' + release.plain(error.message, 200)); finishHook(s);
  }
  async function complete() {
    const previous = s.recovered ? await archive.inspect(fs.readFileSync(s.backup), s.previousVersion) : null;
    stage = s.recovered ? 'verify-backup' : 'verify-target';
    archive.verifyInstalled(previous ? previous.inventory : s.targetInventory, s.directories);
    stage = s.recovered ? 'health-backup' : 'health-target';
    await ops.health(s.recovered ? s.previousVersion : s.artifact.version);
    save('awaiting-entry', 'Open Home+ to finish checking the installation.');
    stage = 'native-entry';
    while (!await ops.enter()) { await wait(2000); }
    save('restoring-home', 'Checking the requested Home settings');
    stage = 'restore-home';
    await ops.restoreHome();
    s.active = false;
    save(s.recovered ? 'failed' : 'completed', s.recovered ? 'Update failed. The previous Home+ build was restored.' : 'Home+ updated successfully');
    finishHook(s);
  }
  try {
    if (s.cancelled) { await cancel(new Error('Update cancelled')); return; }
    const pending = s.installerPending === true || ['installing', 'uncertain'].includes(s.phase);
    if (pending && (!generationId(s.installBoot) || s.installBoot === boot)) {
      // A killed helper cannot record uncertainty. Both installers may still be running.
      // Legacy/missing identity is baselined without authorising a rollback.
      // Only a later verified runtime generation can clear this quarantine.
      if (!generationId(s.installBoot)) s.installBoot = boot;
      save('uncertain', 'Installer completion is unknown. Recovery needs a full webOS restart, not standby. Your backup is retained.');
      try { await ops.fallback(); } catch (e) { /* retain the independent Home guard and recovery hook */ }
      return;
    }
    recovery.atomic(file + '.ready', { id: s.id, pid: process.pid });
    if (s.phase === 'prepared') {
      if (await ops.foreground() !== APP) { await cancel(new Error('Home+ is no longer in front')); return; }
      stage = 'verify-staged';
      const target = fs.readFileSync(s.targetFile);
      if (archive.hash(target) !== s.artifact.sha256 || target.length !== s.artifact.size) throw new Error('Staged package hash changed');
      await archive.inspect(target, s.artifact.version);
      const backup = fs.readFileSync(s.backup);
      if (archive.hash(backup) !== s.backupHash) throw new Error('Recovery backup hash changed');
      const prior = await archive.inspect(backup, s.previousVersion);
      archive.verifyInstalled(prior.inventory, s.directories);
      if (await ops.foreground() !== APP) throw new Error('Update cancelled because Home+ is no longer in front');
      replacing = true;
      await install('target');
      stage = 'verify-target'; archive.verifyInstalled(s.targetInventory, s.directories);
      stage = 'elevate-target'; await ops.elevate();
      stage = 'health-target'; await ops.health(s.artifact.version);
      save('awaiting-entry', 'Installed. Open Home+ to finish checking the update.');
    } else if (!['awaiting-entry', 'restoring-home'].includes(s.phase)) {
      throw new Error('Interrupted installation requires recovery');
    }
    await complete();

  } catch (error) {
    if (!s.failure) failure('failure', error);
    if (error.installerUncertain) {
      try { await ops.fallback(); } catch (e) { /* the existing independent Home guard also remains effective */ }
      save('uncertain', 'Installer completion is unknown. Recovery needs a full webOS restart, not standby. Your backup is retained.');
      return;
    }
    if (!replacing) {
      await cancel(error); return;
    }
    try {
      save('recovering', 'Update did not complete. Restoring the verified previous package.');
      stage = 'fallback'; await ops.fallback();
      stage = 'verify-backup-archive';
      const backup = fs.readFileSync(s.backup);
      if (archive.hash(backup) !== s.backupHash) throw new Error('Recovery backup hash mismatch');
      const old = await archive.inspect(backup, s.previousVersion);
      await install('backup');
      stage = 'verify-backup'; archive.verifyInstalled(old.inventory, s.directories);
      stage = 'elevate-backup'; await ops.elevate();
      stage = 'health-backup'; await ops.health(s.previousVersion);
      s.recovered = true;
      save('awaiting-entry', 'Previous build restored. Open Home+ to finish checking recovery.');
      await complete();
    } catch (rollback) {
      failure('recoveryFailure', rollback);
      try { await ops.fallback(); } catch (ignored) { /* independent default Home guard also remains installed */ }
      if (rollback.installerUncertain) s.phase = 'uncertain';
      save(rollback.installerUncertain ? 'uncertain' : 'recovering', 'Recovery needs attention. The verified backup and recovery hook have been retained.');
      throw new Error('Update recovery could not complete');
    }
  } finally { releaseWorker(); }
}
function checkSpace(df, bytes, artifactSize) {
  const line = df.trim().split('\n').pop().trim().split(/\s+/), available = Number(line[line.length - 3]) * 1024;
  if (!Number.isFinite(available) || available < bytes * 3 + artifactSize * 3 + 32 * 1024 * 1024) throw new Error('Not enough free space for installation and recovery');
}
async function prepare(options) {
  if (process.platform !== 'linux' || !options.isRoot()) throw new Error('Updating Home+ requires elevated webOS');
  const dir = options.stateDir;
  privateDir(dir);
  const file = path.join(dir, 'transaction.json');
  const previous = fs.existsSync(file) ? load(file) : null;
  if (previous && previous.active) throw new Error('An update transaction is already running');
  const boot = bootId(); // refuse unsupported recovery identity before changing Home or replacing packages
  let lock = file + '.worker';
  while (fs.existsSync(lock)) {
    let owner;
    try { owner = lockRecord(lock).owner; } catch (e) { if (e.code === 'ENOENT') break; throw e; }
    if (workerAlive(owner, boot)) throw new Error('The previous update helper is still finishing');
    lock = successor(file + '.worker', owner);
  }
  if (options.externalBusy && options.externalBusy()) throw new Error('Another Home+ setup operation is running');
  fs.accessSync(ELEVATE, fs.constants.X_OK);
  fs.accessSync(process.execPath, fs.constants.X_OK);
  if (options.appDir && process.execPath.indexOf(options.appDir + path.sep) === 0) throw new Error('Recovery runtime must be outside the application');
  await runFile('script', ['-q', '-c', 'printf homeplus-update-terminal', '/dev/null']).then(out => { if (out.indexOf('homeplus-update-terminal') < 0) throw new Error('Installer terminal is unavailable'); });
  fs.accessSync('/usr/bin/luna-send', fs.constants.X_OK);
  const hbc = await recovery.accepted(recovery.luna, HBC + 'getConfiguration', {});
  if (hbc.root !== true) throw new Error('Homebrew Channel is not elevated');
  await recovery.accepted(recovery.luna, HBC + 'exec', { command: quote(process.execPath) + ' --version' });
  const directories = [options.appDir, options.serviceDir, options.packageDir];
  if (directories.some(d => !d || !path.isAbsolute(d))) throw new Error('Installed package locations are unknown');
  if (directories.some(d => options.configPath === d || options.configPath.indexOf(d + path.sep) === 0)) throw new Error('Home+ settings must be outside the replaced package');
  if (options.readConfig().defaultHomeEnabled === true) {
    const guard = recovery.load(options.homeStateFile);
    if (!guard.active || guard.deadline !== null) throw new Error('Default Home recovery is not ready');
  }
  const files = archive.snapshot(directories);
  const bytes = Object.keys(files).reduce((sum, name) => sum + files[name].data.length, 0);
  const df = await runFile('df', ['-Pk', dir]);
  checkSpace(df, bytes, options.artifact.size);
  const backup = archive.pack(files, options.version), verified = await archive.inspect(backup, options.version);
  archive.verifyInstalled(verified.inventory, directories);
  const data = await release.download(options.artifact.url, options.artifact.size);
  if (data.length !== options.artifact.size || archive.hash(data) !== options.artifact.sha256) throw new Error('Downloaded package hash or size mismatch');
  const target = await archive.inspect(data, options.artifact.version);
  const installedBytes = Object.keys(target.files).reduce((sum, name) => sum + target.files[name].data.length, 0);
  checkSpace(await runFile('df', ['-Pk', options.appDir]), bytes, installedBytes);
  if (options.readConfig().defaultHomeEnabled === true) {
    const app = JSON.parse(target.files[archive.roots[0] + '/appinfo.json'].data.toString());
    if (!Array.isArray(app.appCategory) || !app.appCategory.includes('home')) throw new Error('This package cannot preserve the requested default Home app');
  }
  archive.verifyInstalled(verified.inventory, directories);
  if (!options.foreground()) throw new Error('Update cancelled because Home+ is no longer in front');
  const id = require('crypto').randomBytes(12).toString('hex');
  const s = { schemaVersion: 1, id, active: true, phase: 'prepared', message: 'Preparing independent installation', previousVersion: options.version,
    artifact: options.artifact, backup: path.join(dir, id + '-previous.ipk'), targetFile: path.join(dir, id + '-target.ipk'), backupHash: archive.hash(backup), targetInventory: target.inventory,
    configPath: options.configPath, configBackup: path.join(dir, id + '-config.json'), directories, defaultHome: options.readConfig().defaultHomeEnabled === true,
    homeStateFile: options.homeStateFile, homeBackup: path.join(dir, id + '-home.json'), hook: path.join(options.initDir, '01-homeplus-update'), boot: path.join(dir, 'boot.sh') };
  if (fs.existsSync(s.hook) && !ownHook(s)) throw new Error('Update recovery hook path is already in use');
  if (options.homeStateFile && fs.existsSync(options.homeStateFile)) recovery.durable(s.homeBackup, fs.readFileSync(options.homeStateFile));
  const startup = path.join(options.initDir, '50-homeplus');
  if (fs.existsSync(startup) && !fs.lstatSync(startup).isSymbolicLink()) throw new Error('Home+ startup hook must use the supported package link before updating');
  s.startup = fs.existsSync(startup) ? { target: fs.readlinkSync(startup) } : null;
  recovery.durable(s.backup, backup); recovery.durable(s.targetFile, data); recovery.durable(s.configBackup, fs.readFileSync(s.configPath));
  for (const name of ['update-worker.js', 'update-archive.js', 'update-release.js', 'default-home-guard.js', 'net.js', 'mkdir.js']) recovery.durable(path.join(dir, name), fs.readFileSync(path.join(__dirname, name)));
  recovery.durable(s.boot, '#!/bin/sh\n' + quote(process.execPath) + ' ' + quote(path.join(dir, 'update-worker.js')) + ' --start ' + quote(file) + '\n', 0o700);
  if (!ownHook(s)) fs.symlinkSync(s.boot, s.hook); recovery.syncDir(options.initDir);
  recovery.atomic(file, s);
  // The new private backup has been verified and flushed before retiring the previous transaction's files.
  if (previous) {
    for (const suffix of ['-previous.ipk', '-target.ipk', '-config.json', '-home.json']) {
      try { fs.unlinkSync(path.join(dir, previous.id + suffix)); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    }
    recovery.syncDir(dir);
    for (const suffix of ['-target-elevation.json', '-target-activation.json', '-target-ping.json', '-backup-elevation.json', '-backup-activation.json', '-backup-ping.json']) {
      try { fs.unlinkSync(path.join(dir, previous.id + suffix)); } catch (ignored) { /* optional evidence must not prevent the next update */ }
    }
  }
  try { if (options.suspendHome) await options.suspendHome(); }
  catch (e) {
    require('./updates').restoreConfig(s, options.writeConfig);
    s.active = false; s.phase = 'error'; s.message = 'Update stopped before replacement'; recovery.atomic(file, s); finishHook(s);
    if (options.resumeHome) await options.resumeHome();
    throw e;
  }
  await recovery.accepted(recovery.luna, HBC + 'exec', { command: quote(process.execPath) + ' ' + quote(path.join(dir, 'update-worker.js')) + ' --start ' + quote(file) });
  const end = Date.now() + 5000;
  while (Date.now() < end) {
    try { const ready = read(file + '.ready'); if (ready.id === id) { process.kill(ready.pid, 0); return; } } catch (e) { /* worker starting */ }
    await wait(50);
  }
  throw new Error('Independent installer did not start; recovery hook remains armed');
}
function start(file) {
  const fd = fs.openSync(file + '.log', 'a', 0o600);
  const child = spawn(process.execPath, [path.join(path.dirname(file), 'update-worker.js'), '--run', file], { detached: true, stdio: ['ignore', fd, fd], cwd: path.dirname(file), env: { PATH: '/usr/bin:/bin:/usr/sbin:/sbin', HOME: '/home/root' } });
  child.unref(); fs.closeSync(fd);
}
module.exports = { bootId, prepare, run, start, load, production, finalReply, installSubscription, checkSpace };
if (require.main === module) {
  const mode = process.argv[2], file = process.argv[3];
  if (mode === '--start') start(file);
  else if (mode === '--run') run(file).catch(e => { console.error(e.message); process.exitCode = 1; });
  else { console.error('Invalid update worker invocation'); process.exitCode = 1; }
}
