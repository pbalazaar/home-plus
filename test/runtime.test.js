/* The shipped service must parse and remove its files on the TV's Node, independently of the
 * newer Node running node:test. CI selects the target with HOMEPLUS_SERVICE_NODE. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.join(__dirname, '..');
const NODE = process.env.HOMEPLUS_SERVICE_NODE || process.execPath;
const run = (args, options = {}) => {
  const r = spawnSync(NODE, args, { cwd: ROOT, encoding: 'utf8', timeout: 15000, ...options });
  assert.equal(r.error, undefined);
  assert.equal(r.status, 0, r.stderr || r.stdout);
};

test('runtime: every shipped service module parses on the target Node', () => {
  for (const file of fs.readdirSync(path.join(ROOT, 'service')).filter((f) => f.endsWith('.js'))) {
    run(['--check', path.join(ROOT, 'service', file)]);
  }
});

for (const mode of ['sync', 'async']) {
  test(`runtime: ${mode} cleanup preserves files outside the removed tree on the target Node`, () => {
    run([path.join(__dirname, 'fixtures', 'remove.js'), mode]);
  });
  test(`runtime: ${mode} cleanup terminates on permission errors on the target Node`, {
    skip: process.platform === 'win32' && 'needs Unix directory permissions'
  }, () => {
    /* Root can unlink despite mode 0500: use an unprivileged child in root-run containers. */
    const options = process.getuid && process.getuid() === 0 ? { uid: 65534, gid: 65534 } : {};
    run([path.join(__dirname, 'fixtures', 'remove-errors.js'), mode], options);
  });
}
