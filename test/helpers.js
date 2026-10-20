/* Helpers for Home+'s tests (Node's built-in test runner, no dependencies).
 * Nothing in test/ ships to the TV: tools/build.mjs packages service/ and native/ only. */
'use strict';

const http = require('http');

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

module.exports = { mockServer, configStore, quiet, sleep, until, freshRequire };
