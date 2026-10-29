/* Helpers for Home+'s tests (Node's built-in test runner, no dependencies).
 * Nothing in test/ ships to the TV: tools/build.mjs packages service/ and native/ only. */
'use strict';

const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');
const { spawn } = require('child_process');
const { URL } = require('url');

const ROOT = path.join(__dirname, '..');

/* A local HTTP server standing in for an online service. route(req, url, body) returns
 * { status, json } or { status, body, type } (either with headers too), { hang: true } to never
 * answer, { cut: text } to send only text of a longer reply and then hang up, or nothing for a 404.
 * Every request is recorded in .hits. */
async function mockServer(route) {
  const hits = [];
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', async () => {
      const url = new URL(req.url, 'http://127.0.0.1');
      hits.push({ method: req.method, path: url.pathname, query: url.searchParams, headers: req.headers, body });
      const r = (await route(req, url, body)) || { status: 404, json: {} };
      if (r.hang) return;
      if (r.cut !== undefined) { /* it says 100 bytes more are coming, and closes the connection instead */
        res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(r.cut) + 100 });
        res.write(r.cut);
        setTimeout(() => res.destroy(), 50);
        return;
      }
      res.writeHead(r.status || 200, Object.assign({ 'Content-Type': r.type || 'application/json' }, r.headers));
      res.end(r.json !== undefined ? JSON.stringify(r.json) : r.body || '');
    });
  });
  const sockets = new Set(); /* (Node 8 has no closeAllConnections) */
  server.on('connection', (s) => { sockets.add(s); s.on('close', () => sockets.delete(s)); });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    hits,
    close: () => new Promise((resolve) => {
      for (const s of sockets) s.destroy();
      server.close(resolve);
    })
  };
}

/* An in-memory config.json, as the service's readConfig/writeConfig see it. */
function configStore(initial = {}) {
  let cfg = JSON.parse(JSON.stringify(initial));
  return {
    readConfig: () => JSON.parse(JSON.stringify(cfg)),
    writeConfig: (c) => { cfg = JSON.parse(JSON.stringify(c)); },
    get: () => cfg
  };
}

const quiet = () => {};
/* A list's last item (Node 8 has no .at(-1)) */
const last = (list) => list[list.length - 1];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/* Waits until check() is true, polling every 50 ms; fails after ms. */
async function until(check, ms = 8000, what = 'condition') {
  const end = Date.now() + ms;
  while (!(await check())) {
    if (Date.now() > end) throw new Error(`timed out waiting for ${what}`);
    await sleep(50);
  }
}

/* Loads a service module fresh (modules read their HOMEPLUS_* settings when loaded). */
function freshRequire(file) {
  const p = require.resolve(file);
  delete require.cache[p];
  return require(p);
}

function freePort() {
  return new Promise((resolve) => {
    const s = net.createServer().listen(0, '127.0.0.1', () => {
      const p = s.address().port;
      s.close(() => resolve(p));
    });
  });
}

/* What the service has logged, once its log is a file (a test may put a link or a FIFO at its name first) */
function logged(file) {
  try { return fs.lstatSync(file).isFile() ? fs.readFileSync(file, 'utf8') : ''; } catch (e) { return ''; }
}

/* The real service.js on the preview harness's stand-in webOS (tools/native-preview/harness.js;
 * dir holds its apps.txt and pictures), on a port of its own, with its config in dir/config and its
 * log in dir/service.log, so no root is needed; env adds HOMEPLUS_* settings. HOMEPLUS_SERVICE_NODE
 * runs it with another Node, such as webOS 22/23's Node 12 (CI does). Resolves once it answers, with
 * call() for requests as the launcher makes them (token, Host 127.0.0.1) unless told otherwise, and stop().
 * Test files run in parallel: another's service or server can take the port freePort() found before
 * this one listens on it. The service then only logs that it couldn't (on the TV, the old Home+'s
 * service has its port), and is started again on another port. */
async function startService(dir, env = {}) {
  const logFile = env.HOMEPLUS_LOG || path.join(dir, 'service.log');
  for (let tries = 1; ; tries++) {
    try {
      return await launch(dir, env, logFile);
    } catch (e) {
      if (!e.taken || tries === 3) throw e; /* (three in a row isn't chance) */
    }
  }
}

/* One try at startService. The service is asked whether it's up only once its log says it listens:
 * till then, whoever has its port would answer. */
async function launch(dir, env, logFile) {
  const port = await freePort();
  const child = spawn(process.env.HOMEPLUS_SERVICE_NODE || process.execPath,
    [path.join(ROOT, 'tools/native-preview/harness.js'), dir, path.join(ROOT, 'service')], {
      env: Object.assign({}, process.env, {
        NODE_PATH: path.join(ROOT, 'tools/native-preview/stub'),
        HOMEPLUS_CONFIG_DIR: path.join(dir, 'config'),
        HOMEPLUS_TOKEN_FILE: path.join(dir, 'api-token'),
        HOMEPLUS_LOG: logFile,
        HOMEPLUS_PORT: String(port),
        HOMEPLUS_PREVIEW: '1'
      }, env),
      stdio: ['ignore', 'pipe', 'pipe']
    });
  let output = '';
  child.stdout.on('data', (d) => { output += d; });
  child.stderr.on('data', (d) => { output += d; });
  const stopped = () => child.exitCode !== null || child.signalCode !== null; /* (killed: no exit code) */
  const svc = {
    port,
    token: '',
    call(method, p, { body, headers = {}, auth = true } = {}) {
      return new Promise((resolve, reject) => {
        const data = body === undefined ? '' : JSON.stringify(body);
        const h = Object.assign(auth ? { 'X-Homeplus-Token': svc.token } : {}, data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {}, headers);
        const req = http.request({ host: '127.0.0.1', port, path: p, method, headers: h }, (res) => {
          let buf = '';
          res.on('data', (c) => { buf += c; });
          res.on('end', () => {
            let json = null;
            try { json = JSON.parse(buf); } catch (e) { /* not JSON */ }
            resolve({ status: res.statusCode, json });
          });
        });
        req.on('error', reject);
        req.end(data || undefined);
      });
    },
    async stop() {
      if (stopped()) return;
      const gone = new Promise((resolve) => child.once('exit', resolve)); /* Windows won't delete files the service still has open */
      child.kill();
      await gone;
    }
  };
  let taken = false;
  await until(async () => {
    const log = logged(logFile);
    /* (the line ends with the port: the try before this one leaves its own until this service empties the log) */
    if (new RegExp(`image server error .*:${port}$`, 'm').test(log)) {
      taken = true;
      throw new Error(`port ${port} was taken`);
    }
    if (stopped()) throw new Error(`the service stopped (${child.signalCode || `exit code ${child.exitCode}`})`);
    if (!log.includes(`image server listening on ${port}\n`)) return false;
    try { return (await svc.call('GET', '/api/status', { auth: false })).status === 200; } catch (e) { return false; }
  }, 15000, 'the service to start').catch(async (e) => {
    await svc.stop();
    const error = new Error(`${e.message}. Its output:\n${output}\nIts log:\n${logged(logFile)}`);
    if (taken) error.taken = true; /* (startService tries another port) */
    throw error;
  });
  svc.token = fs.readFileSync(path.join(dir, 'api-token'), 'utf8').trim();
  return svc;
}

module.exports = { mockServer, configStore, quiet, sleep, last, until, freshRequire, startService };
