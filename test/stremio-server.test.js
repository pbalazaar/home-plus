/* Stremio's server, stopped once Stremio has closed: against a stand-in /proc and SAM. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { quiet, freshRequire, sleep } = require('./helpers');

const proc = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-proc-'));
test.after(() => fs.rmSync(proc, { recursive: true, force: true }));
process.env.HOMEPLUS_PROC = proc;
process.env.HOMEPLUS_STREMIO_GRACE_MS = '60'; /* (on the TV: 10 s) */

/* a process in the stand-in /proc, with its command line as Linux writes it */
function processes(list) {
  for (const name of fs.readdirSync(proc)) fs.rmSync(path.join(proc, name), { recursive: true, force: true });
  for (const [pid, cmdline] of Object.entries(list)) {
    fs.mkdirSync(path.join(proc, pid));
    fs.writeFileSync(path.join(proc, pid, 'cmdline'), cmdline);
  }
}

/* the module, with SAM saying which apps run (null: no answer) and the processes it stops */
function server(running = []) {
  const killed = [], sam = { running };
  const luna = async (uri) => {
    assert.equal(uri, 'luna://com.webos.applicationManager/running');
    return sam.running ? { returnValue: true, running: sam.running.map((id) => ({ id })) } : { returnValue: false };
  };
  const s = freshRequire('../service/stremio-server')({ luna, log: quiet, kill: (pid, sig) => killed.push([pid, sig]) });
  return Object.assign(s, { killed, sam });
}

const SERVER = 'io.strem.tv.server' + ' '.repeat(40); /* Node's title, padded over its old command line */

test('stremio: closed and still closed, its server is stopped, and nothing else', async () => {
  processes({ 120: SERVER, 121: 'node\0/usr/bin/other.js\0', 122: 'io.strem.tv.server.old\0', self: 'x' });
  const s = server(['youtube.leanback.v4']);
  s.onLife('io.strem.tv', 'close');
  assert.deepEqual(s.killed, [], 'not at once: Stremio may be opening again');
  await sleep(150);
  assert.deepEqual(s.killed, [[120, 'SIGTERM']]);
});

test('stremio: opened again within the grace, its server stays', async () => {
  processes({ 130: SERVER });
  const s = server([]);
  s.onLife('io.strem.tv', 'stop');
  s.onLife('io.strem.tv', 'launch');
  await sleep(150);
  assert.deepEqual(s.killed, []);
});

test('stremio: still running (in the background), or SAM not saying, its server stays', async () => {
  processes({ 140: SERVER });
  const running = server(['io.strem.tv']);
  running.onLife('io.strem.tv', 'close');
  const unknown = server(null);
  unknown.onLife('io.strem.tv', 'close');
  await sleep(150);
  assert.deepEqual(running.killed, []);
  assert.deepEqual(unknown.killed, []);
});

test('stremio: other apps closing do nothing', async () => {
  processes({ 150: SERVER });
  const s = server([]);
  s.onLife('netflix', 'close');
  s.onLife('io.strem.tv', 'foreground');
  await sleep(150);
  assert.deepEqual(s.killed, []);
});

test('stremio: a server left running when the service starts is stopped, Stremio not open', async () => {
  processes({ 160: SERVER });
  const s = server(['airplay']);
  s.check();
  await sleep(150);
  assert.deepEqual(s.killed, [[160, 'SIGTERM']]);
});

test('stremio: no server running, or no /proc, is nothing to do', async () => {
  processes({});
  const s = server([]);
  s.check();
  await sleep(150);
  assert.deepEqual(s.killed, []);
  fs.rmSync(proc, { recursive: true, force: true });
  const gone = server([]);
  gone.check();
  await sleep(150);
  assert.deepEqual(gone.killed, []);
  fs.mkdirSync(proc);
});
