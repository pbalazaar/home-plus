/* The Node 12 recursive-rmdir bug needs a real permission failure, under a non-root user. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const remove = require('../../service/remove');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-remove-errors-'));
const locked = path.join(dir, 'locked');
fs.mkdirSync(locked);
fs.writeFileSync(path.join(locked, 'keep'), 'permission denied');
fs.chmodSync(locked, 0o500);
const rm = process.argv[2] === 'sync'
  ? async () => remove.rmSync(dir, { recursive: true, force: true })
  : () => new Promise((resolve, reject) => remove.rm(dir, { recursive: true, force: true }, (e) => e ? reject(e) : resolve()));

(async () => {
  try {
    let error;
    try { await rm(); } catch (e) { error = e; }
    // Node 16's native rmSync can leave a protected tree without throwing, and Node 26's reports
    // ENOTEMPTY for it. Preserve native behavior there; our Node 8 and 12 fallback must report
    // the permission error, and neither may hang.
    const native = process.argv[2] === 'sync' ? fs.rmSync : fs.rm;
    if (!native) assert.ok(error, 'the Node 8/12 fallback reports permission errors');
    if (error) assert.ok(error.code === 'EACCES' || error.code === 'EPERM' || (native && error.code === 'ENOTEMPTY'), error.message);
    assert.equal(fs.readFileSync(path.join(locked, 'keep'), 'utf8'), 'permission denied');
  } finally {
    fs.chmodSync(locked, 0o700);
    fs.unlinkSync(path.join(locked, 'keep'));
    fs.rmdirSync(locked);
    fs.rmdirSync(dir);
  }
})().catch((e) => { console.error(e); process.exitCode = 1; });
