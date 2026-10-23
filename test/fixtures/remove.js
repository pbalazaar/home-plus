/* Runs on Node 12 too: exercise the real filesystem, including links out of a tree. */
'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const remove = require('../../service/remove');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-remove-'));
const file = (name) => path.join(dir, name);
const mode = process.argv[2];
const rm = mode === 'sync'
  ? async (p, options) => remove.rmSync(p, options)
  : (p, options) => new Promise((resolve, reject) => {
    let returned = false;
    remove.rm(p, options, (e) => {
      assert.ok(returned, 'the callback is asynchronous');
      if (e) reject(e); else resolve();
    });
    returned = true;
  });

(async () => {
  try {
    fs.writeFileSync(file('regular'), 'remove this');
    await rm(file('regular'), { force: true });
    assert.ok(!fs.existsSync(file('regular')));
    await rm(file('missing'), { force: true });
    await rm(file('missing-tree'), { recursive: true, force: true });
    await assert.rejects(() => rm(file('missing'), { force: false }), { code: 'ENOENT' });

    fs.mkdirSync(file('tree/nested'), { recursive: true });
    fs.writeFileSync(file('tree/nested/data'), 'remove this too');
    fs.mkdirSync(file('outside'));
    fs.writeFileSync(file('outside/keep'), 'keep this');
    if (process.platform !== 'win32') {
      fs.symlinkSync(file('outside'), file('tree/link'), 'dir');
      fs.symlinkSync(file('absent'), file('dangling'));
      await rm(file('dangling'), { force: true });
      assert.throws(() => fs.lstatSync(file('dangling')), { code: 'ENOENT' });
      fs.symlinkSync(file('outside'), file('linked-tree'), 'dir');
      await rm(file('linked-tree'), { recursive: true, force: true });
      assert.throws(() => fs.lstatSync(file('linked-tree')), { code: 'ENOENT' });
    }
    await assert.rejects(() => rm(file('tree'), { force: true }));
    assert.equal(fs.readFileSync(file('tree/nested/data'), 'utf8'), 'remove this too');
    await rm(file('tree'), { recursive: true, force: true });
    assert.ok(!fs.existsSync(file('tree')));
    assert.equal(fs.readFileSync(file('outside/keep'), 'utf8'), 'keep this');
  } finally {
    /* The runner always cleans up, even before the compatibility helper exists. */
    if (fs.rmSync) fs.rmSync(dir, { recursive: true, force: true });
    else fs.rmdirSync(dir, { recursive: true });
  }
})().catch((e) => { console.error(e); process.exitCode = 1; });
