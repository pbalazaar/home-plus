'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const remove = require('../service/remove');
const fs = require('fs'), os = require('os'), path = require('path'), net = require('net');
const { spawn, execFile } = require('child_process');
const { until, sleep, guardShellFixture } = require('./helpers');
const recovery = require('../service/default-home-guard');
const createHome = require('../service/default-home');
const APP = 'com.github.pbalazaar.homeplus', LG = 'com.webos.app.home';
function fixture(deadline, quoted) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), quoted ? "homeplus guard's " : 'homeplus-independent-')), app = path.join(dir, 'app');
  fs.mkdirSync(app);
  fs.writeFileSync(path.join(app, 'appinfo.json'), JSON.stringify({ id: APP, appCategory: ['home'] }));
  fs.writeFileSync(path.join(app, 'homeplus'), 'native');
  const helper = path.join(dir, 'guard.js'), binary = path.join(dir, "luna's fixture"), file = path.join(dir, 'state.json');
  fs.copyFileSync(path.join(__dirname, '../service/default-home-guard.js'), helper);
  const preload = guardShellFixture(dir);
  fs.copyFileSync(path.join(__dirname, 'fixtures/system-home-luna.js'), binary); fs.chmodSync(binary, 0o755);
  const config = path.join(dir, 'config.json'), model = path.join(dir, 'model.json');
  recovery.atomic(config, { defaultHomeEnabled: true, homeOverride: true, homeMode: 'watch', account: 'keep' });
  recovery.atomic(model, { defaults: { home: APP, browser: 'changed.browser' }, calls: [] });
  recovery.atomic(file, { version: 1, active: true, deadline, transaction: 'owned', applicationDir: app,
    configPath: config, stockHome: LG, before: { browser: 'old.browser' }, previous: { homeMode: 'watch', homeOverride: false } });
  const children = [], detached = [];
  const read = (f) => JSON.parse(fs.readFileSync(f, 'utf8'));
  function start(mode) {
    const child = spawn(process.env.HOMEPLUS_SERVICE_NODE || process.execPath, ['-r', preload, helper, mode, file], {
      env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: binary, HOMEPLUS_MOCK_LUNA_STATE: model }),
      stdio: ['ignore', 'pipe', 'pipe']
    });
    children.push(child); let output = '';
    child.stdout.on('data', (x) => { output += x; }); child.stderr.on('data', (x) => { output += x; });
    const done = new Promise((resolve, reject) => {
      child.on('error', reject); child.on('exit', (code) => code === 0 ? resolve(output) : reject(new Error(output)));
    });
    return { child, done };
  }
  return { dir, app, file, config, model, read, start, detached, children,
    async close() {
      const s = read(file); s.active = false; recovery.atomic(file, s);
      for (const c of children) if (c.exitCode === null && c.signalCode === null) {
        const exited = new Promise((resolve) => c.once('exit', resolve)); c.kill('SIGTERM'); await exited;
      }
      for (const pid of detached) {
        try { process.kill(pid, 'SIGTERM'); } catch (e) { if (e.code !== 'ESRCH') throw e; }
        await until(() => {
          try { return /\) Z /.test(fs.readFileSync('/proc/' + pid + '/stat', 'utf8')); }
          catch (e) { if (e.code === 'ENOENT') return true; throw e; }
        }, 5000, 'the detached guard to stop writing');
      }
      remove.rmSync(dir, { recursive: true, force: true });
    }
  };
}
test('independent boot recovery rejects an unfinished trial immediately', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(Date.now() + 120000);
  try {
    const output = await t.start('--start').done; t.detached.push(JSON.parse(output).pid);
    await until(() => !t.read(t.file).active, 7000);
    assert.deepEqual(t.read(t.model).defaults, { browser: 'changed.browser' });
    assert.deepEqual(t.read(t.config), { homeMode: 'watch', homeOverride: false, account: 'keep' });
  } finally { await t.close(); }
});
test('permanent recovery survives its starter and handles removal outside the app', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(null);
  try {
    const output = await t.start('--start').done; t.detached.push(JSON.parse(output).pid);
    await until(() => fs.existsSync(t.file + '.ready'));
    await sleep(300); assert.equal(t.read(t.file).active, true);
    fs.unlinkSync(path.join(t.app, 'homeplus'));
    await until(() => !t.read(t.file).active, 10000);
    assert.deepEqual(t.read(t.model).defaults, { browser: 'changed.browser' });
    assert.equal(t.read(t.config).homeOverride, false);
    assert.equal(t.read(t.config).account, 'keep');
  } finally { await t.close(); }
});
test('a brief package update is tolerated, but a downgraded non-Home manifest is restored', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(null);
  try {
    const g = t.start('--watch');
    await until(() => fs.existsSync(t.file + '.ready'));
    fs.unlinkSync(path.join(t.app, 'homeplus'));
    await sleep(300); fs.writeFileSync(path.join(t.app, 'homeplus'), 'updated native');
    await sleep(2200); assert.equal(t.read(t.file).active, true);
    recovery.atomic(path.join(t.app, 'appinfo.json'), { id: APP });
    await until(() => !t.read(t.file).active, 10000); await g.done;
    assert.deepEqual(t.read(t.model).defaults, { browser: 'changed.browser' });
    assert.equal(t.read(t.config).account, 'keep');
  } finally { await t.close(); }
});

test('a replaced transaction retires its sleeping watcher without touching the new default', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(null);
  try {
    const g = t.start('--watch');
    await until(() => fs.existsSync(t.file + '.ready'));
    const s = t.read(t.file); s.transaction = 'replacement'; recovery.atomic(t.file, s);
    await g.done;
    assert.equal(t.read(t.file).active, true);
    assert.equal(t.read(t.model).defaults.home, APP);
    assert.equal(t.read(t.model).calls.length, 0);
  } finally { await t.close(); }
});
test('replacement during a delayed recovery health check cannot change config or defaults', { skip: process.platform !== 'linux' }, async () => {
  const t = fixture(Date.now() - 1);
  try {
    const m = t.read(t.model); m.healthDelay = 750; recovery.atomic(t.model, m);
    const g = t.start('--watch');
    await until(() => t.read(t.model).calls.length);
    const s = t.read(t.file); s.transaction = 'replacement'; recovery.atomic(t.file, s);
    await g.done;
    assert.equal(t.read(t.file).active, true);
    assert.equal(t.read(t.config).defaultHomeEnabled, true);
    assert.equal(t.read(t.model).defaults.home, APP);
    assert.equal(t.read(t.model).calls.length, 1);
  } finally { await t.close(); }
});

for (const mode of ['live', 'standalone', 'unsupported shell']) {
  test('guard isolates real inherited sockets through the ' + mode + ' producer', { skip: process.platform !== 'linux' }, async () => {
    const t = fixture(null, true), socketPath = path.join(t.dir, 'peer');
    const server = net.createServer(), connection = new Promise((resolve) => server.once('connection', resolve));
    let socket, peer, service;
    try {
      await new Promise((resolve, reject) => { server.once('error', reject); server.listen(socketPath, resolve); });
      socket = net.connect(socketPath); peer = await connection;
      let received = '', ended = false;
      peer.on('data', (data) => { received += data; }); peer.on('end', () => { ended = true; });
      // Explicit high mappings reproduce native non-CLOEXEC sockets beyond Node's startup scan gap.
      const stdio = Array(143).fill('ignore'); stdio[1] = 'pipe'; stdio[2] = 'pipe';
      stdio[20] = socket._handle.fd; stdio[64] = socket._handle.fd; stdio[142] = socket._handle.fd;
      const source = `
        const fs = require('fs'), path = require('path'), cp = require('child_process');
        const input = JSON.parse(process.argv[1]);
        const recovery = require(input.module), createHome = require(input.home);
        const read = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));
        const descriptors = fs.readdirSync('/proc/self/fd').map((entry) => {
          const fd = Number(entry);
          try { return { fd, target: fs.readlinkSync('/proc/self/fd/' + fd),
            flags: parseInt(/^flags:\\s+([0-7]+)/m.exec(fs.readFileSync('/proc/self/fdinfo/' + fd, 'utf8'))[1], 8) }; }
          catch (e) { if (e.code !== 'ENOENT') throw e; }
        }).filter(Boolean);
        recovery.atomic(input.proof, descriptors); fs.writeSync(64, 'before;');
        const keepAlive = setInterval(() => {}, 1000);
        async function run() {
          let pid;
          if (input.mode === 'standalone') {
            const child = cp.spawn(process.execPath, ['-r', input.preload, input.module, '--start', input.file],
              { env: process.env, stdio: ['ignore', 'pipe', 'pipe'] });
            let output = '', error = '';
            child.stdout.on('data', (x) => { output += x; }); child.stderr.on('data', (x) => { error += x; });
            pid = await new Promise((resolve, reject) => child.on('close', (code) =>
              code === 0 ? resolve(JSON.parse(output).pid) : reject(new Error(error))));
          } else {
            const cfg = read(input.config); cfg.defaultHomeEnabled = false; recovery.atomic(input.config, cfg);
            const model = read(input.model); delete model.defaults.home; recovery.atomic(input.model, model);
            const luna = (uri, params) => new Promise((resolve, reject) => cp.execFile(input.binary, [uri, JSON.stringify(params)],
              (error, out) => error ? reject(error) : resolve(JSON.parse(out))));
            const home = createHome({ luna, readConfig: () => read(input.config),
              writeConfig: (cfg) => recovery.atomic(input.config, cfg), configPath: () => input.config,
              appDir: input.app, stateDir: input.dir, initDir: path.join(input.dir, 'init.d'),
              isRoot: () => true, suspend: async () => {}, resume: async () => {}, log: () => {} });
            await home.setEnabled(true);
            pid = read(input.file + '.ready').pid;
          }
          fs.writeSync(64, 'after;'); recovery.atomic(input.started, { pid });
        }
        run().catch((e) => { console.error(e.stack); clearInterval(keepAlive); process.exitCode = 1; });
      `;
      const input = { mode, module: path.join(t.dir, 'guard.js'), home: path.resolve(__dirname, '../service/default-home.js'),
        dir: t.dir, app: t.app, file: t.file, config: t.config, model: t.model,
        binary: path.join(t.dir, "luna's fixture"), proof: path.join(t.dir, 'inherited.json'), started: path.join(t.dir, 'started.json'),
        preload: guardShellFixture(t.dir, mode === 'unsupported shell' ? '/bin/dash' : '/bin/bash') };
      // Ubuntu dash lacks the verified C4 shell's high-FD syntax. Adapt only that fixture capability.
      service = spawn(process.env.HOMEPLUS_SERVICE_NODE || process.execPath, ['-r', input.preload, '-e', source, JSON.stringify(input)], {
        stdio, env: Object.assign({}, process.env, { HOMEPLUS_RECOVERY_LUNA: input.binary, HOMEPLUS_MOCK_LUNA_STATE: t.model })
      });
      t.children.push(service); let errors = '';
      service.stderr.on('data', (data) => { errors += data; }); service.stdout.resume();
      socket.destroy();
      await until(() => fs.existsSync(input.proof), 5000, 'the producer descriptor proof');
      const inherited = t.read(input.proof);
      for (const number of [20, 64, 142]) {
        const fd = inherited.find((entry) => entry.fd === number);
        assert.ok(fd, 'the producer holds FD ' + number);
        assert.match(fd.target, /^socket:\[\d+\]$/);
        assert.equal(fd.flags & 0o2000000, 0, 'the producer really inherited a non-CLOEXEC socket at FD ' + fd.fd);
      }
      if (mode === 'unsupported shell') {
        await until(() => service.exitCode !== null, 7000, 'unsupported closure to fail before readiness');
        assert.equal(service.exitCode, 1); assert.match(errors, /Independent Home recovery did not start/);
        assert.equal(fs.existsSync(t.file + '.ready'), false);
        const refused = /exec: ([0-9]+): not found/.exec(fs.readFileSync(t.file + '.log', 'utf8'));
        assert.ok(refused, 'the shell reports refusing a numeric descriptor');
        const descriptor = Number(refused[1]);
        assert.ok(descriptor > 2 && inherited.some((entry) => entry.fd === descriptor && !(entry.flags & 0o2000000)),
          'the refused descriptor was actually inherited above stdio');
        assert.equal(t.read(t.file).active, false);
        assert.equal(t.read(t.model).defaults.home, undefined);
        assert.equal(t.read(t.config).defaultHomeEnabled, false);
        assert.equal(t.read(t.config).account, 'keep');
        return;
      }
      await until(() => fs.existsSync(t.file + '.ready') || service.exitCode !== null, 7000, 'the real guard readiness');
      assert.equal(service.exitCode, null, errors);
      const ready = t.read(t.file + '.ready'); t.detached.push(ready.pid);
      await until(() => fs.existsSync(input.started) || service.exitCode !== null, 7000, 'the producer launch result');
      assert.equal(service.exitCode, null, errors); assert.equal(t.read(input.started).pid, ready.pid);
      const argv = fs.readFileSync('/proc/' + ready.pid + '/cmdline', 'utf8').split('\0').filter(Boolean);
      assert.deepEqual(argv.slice(1), [input.module, '--watch', t.file]);
      assert.equal(fs.readlinkSync('/proc/' + ready.pid + '/fd/0'), '/dev/null');
      for (const fd of [1, 2]) assert.equal(fs.readlinkSync('/proc/' + ready.pid + '/fd/' + fd), t.file + '.log');
      const held = fs.readdirSync('/proc/' + ready.pid + '/fd').map((fd) => {
        try { return fs.readlinkSync('/proc/' + ready.pid + '/fd/' + fd); } catch (e) { if (e.code !== 'ENOENT') throw e; }
      });
      assert.equal(held.includes(inherited.find((entry) => entry.fd === 20).target), false, 'guard must release the inherited service socket');
      await until(() => received === 'before;after;', 2000, 'the parent socket to remain usable');
      const exited = new Promise((resolve) => service.once('close', resolve)); service.kill('SIGTERM'); await exited;
      await until(() => ended, 2000, 'peer EOF without stopping the independent guard');
      process.kill(ready.pid, 0); assert.equal(t.read(t.file).active, true);
      fs.unlinkSync(path.join(t.app, 'homeplus'));
      await until(() => !t.read(t.file).active, 10000, 'independent recovery after producer exit');
      assert.equal(t.read(t.model).defaults.home, undefined);
      assert.equal(t.read(t.model).defaults.browser, 'changed.browser');
      assert.equal(t.read(t.config).account, 'keep');
    } finally {
      if (socket) socket.destroy(); if (peer) peer.destroy();
      await t.close(); await new Promise((resolve) => server.close(resolve));
    }
  });
}

for (const pauseRead of [1, 2]) {
  test('a restore paused at defaults read ' + pauseRead + ' cannot undo a newly confirmed activation',
    { skip: process.platform !== 'linux' }, async () => {
      const t = fixture(Date.now() - 1);
      try {
        const m = t.read(t.model); m.pauseRead = pauseRead; recovery.atomic(t.model, m);
        if (pauseRead === 1) {
          const s = t.read(t.file); s.before.home = LG; recovery.atomic(t.file, s);
        }
        const g = t.start('--restore');
        await until(() => fs.existsSync(t.model + '.paused'), 5000, 'the old restore to await its defaults read');
        const binary = path.join(t.dir, "luna's fixture");
        const luna = (uri, p) => new Promise((resolve, reject) => {
          execFile(binary, [uri, JSON.stringify(p)], {
            env: Object.assign({}, process.env, { HOMEPLUS_MOCK_LUNA_STATE: t.model }), timeout: 5000
          }, (error, stdout) => error ? reject(error) : resolve(JSON.parse(stdout)));
        });
        const home = createHome({
          luna, readConfig: () => t.read(t.config), writeConfig: (cfg) => recovery.atomic(t.config, cfg),
          configPath: () => t.config, appDir: t.app, stateDir: t.dir, initDir: path.join(t.dir, 'init.d'),
          isRoot: () => true, startGuard: async () => {}, suspend: async () => {}, resume: async () => {}, log: () => {}
        });
        await home.recover();
        await home.setEnabled(true);
        const confirmed = t.read(t.file), cfg = t.read(t.config), current = t.read(t.model);
        assert.notEqual(confirmed.transaction, 'owned');
        assert.equal(confirmed.deadline, null);
        assert.equal(current.defaults.home, APP);
        fs.writeFileSync(t.model + '.resume', '');
        await g.done;
        assert.deepEqual(t.read(t.model).defaults, current.defaults);
        assert.deepEqual(t.read(t.config), cfg);
        assert.deepEqual(t.read(t.file), confirmed);
        assert.equal(t.read(t.model).calls.length, current.calls.length, 'the old transaction issued no further Luna calls');
      } finally { await t.close(); }
    });
}
