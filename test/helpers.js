/* Helpers for Home+'s tests (Node's built-in test runner, no dependencies).
 * Nothing in test/ ships to the TV: tools/build.mjs packages service/ and native/ only. */
'use strict';

const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');
const { spawn } = require('child_process');

const ROOT = path.join(__dirname, '..');

/* A local HTTP server standing in for an online service. route(req, url, body) returns
 * { status, json } or { status, body, type }, { hang: true } to never answer, or nothing
 * for a 404. Every request is recorded in .hits. */
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
      res.writeHead(r.status || 200, { 'Content-Type': r.type || 'application/json' });
      res.end(r.json !== undefined ? JSON.stringify(r.json) : r.body || '');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    hits,
    close: () => new Promise((resolve) => {
      server.closeAllConnections();
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

/* The real service.js on the preview harness's stand-in webOS (tools/native-preview/harness.js;
 * dir holds its apps.txt and pictures), on a port of its own, with its config in dir/config, so no
 * root is needed; env adds HOMEPLUS_* settings. HOMEPLUS_SERVICE_NODE runs it with another Node,
 * such as the TV's Node 16 (CI does). Resolves once it answers, with call() for requests as the
 * launcher makes them (token, Host 127.0.0.1) unless told otherwise, and stop(). */
async function startService(dir, env = {}) {
  const port = await freePort();
  const child = spawn(process.env.HOMEPLUS_SERVICE_NODE || process.execPath,
    [path.join(ROOT, 'tools/native-preview/harness.js'), dir, path.join(ROOT, 'service')], {
      env: Object.assign({}, process.env, {
        NODE_PATH: path.join(ROOT, 'tools/native-preview/stub'),
        HOMEPLUS_CONFIG_DIR: path.join(dir, 'config'),
        HOMEPLUS_TOKEN_FILE: path.join(dir, 'api-token'),
        HOMEPLUS_PORT: String(port),
        HOMEPLUS_PREVIEW: '1'
      }, env),
      stdio: ['ignore', 'pipe', 'pipe']
    });
  let log = '';
  child.stdout.on('data', (d) => { log += d; });
  child.stderr.on('data', (d) => { log += d; });
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
      if (child.exitCode !== null) return;
      const gone = new Promise((resolve) => child.once('exit', resolve)); /* Windows won't delete files the service still has open */
      child.kill();
      await gone;
    }
  };
  await until(async () => {
    if (child.exitCode !== null) throw new Error(`the service stopped (exit code ${child.exitCode})`);
    try { return (await svc.call('GET', '/api/status', { auth: false })).status === 200; } catch (e) { return false; }
  }, 15000, 'the service to start').catch(async (e) => {
    await svc.stop();
    throw new Error(`${e.message}. Its output:\n${log}`);
  });
  svc.token = fs.readFileSync(path.join(dir, 'api-token'), 'utf8').trim();
  return svc;
}

module.exports = { mockServer, configStore, quiet, sleep, until, freshRequire, startService };
