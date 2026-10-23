/* The shipped service must parse and remove its files on the TV's Node, independently of the
 * newer Node running node:test. CI selects the target with HOMEPLUS_SERVICE_NODE. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.join(__dirname, '..');
const NODE = process.env.HOMEPLUS_SERVICE_NODE || process.execPath;
const run = (args, options = {}, node = NODE) => {
  const r = spawnSync(node, args, { cwd: ROOT, encoding: 'utf8', timeout: 15000, ...options });
  assert.equal(r.error, undefined);
  assert.equal(r.status, 0, r.stderr || r.stdout);
};

/* Root can unlink despite mode 0500, so as root a fixture runs as nobody (65534). Nobody may not
 * reach the checkout or the Node (a root-only nvm under /root, say): it gets copies, in /tmp. */
function runAsNobody(fixture, mode) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-runtime-'));
  const files = { node: NODE, 'service/remove.js': path.join(ROOT, 'service', 'remove.js'), [`test/fixtures/${fixture}`]: path.join(__dirname, 'fixtures', fixture) };
  try {
    for (const [to, from] of Object.entries(files)) {
      fs.mkdirSync(path.join(dir, path.dirname(to)), { recursive: true });
      fs.copyFileSync(from, path.join(dir, to), fs.constants.COPYFILE_FICLONE);
    }
    for (const p of ['.', 'service', 'test', 'test/fixtures', ...Object.keys(files)]) fs.chmodSync(path.join(dir, p), 0o755); /* (whatever the umask) */
    run([`test/fixtures/${fixture}`, mode], { cwd: dir, uid: 65534, gid: 65534 }, path.join(dir, 'node'));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

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
    if (process.getuid && process.getuid() === 0) runAsNobody('remove-errors.js', mode);
    else run([path.join(__dirname, 'fixtures', 'remove-errors.js'), mode]);
  });
}
