/* The Settings and Input keys' scripts (service/*-key.sh), run by LG Input Hook as root: they pass
 * the API token to curl on its input, never on its command line, where any process on the TV could
 * read it (/proc/<pid>/cmdline). Each script, pointed at a stand-in app folder and service. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, spawnSync } = require('child_process');
const { mockServer } = require('./helpers');

const ROOT = path.join(__dirname, '..');
const APP = '/media/developer/apps/usr/palm/applications/com.github.pbalazaar.homeplus';
const TOKEN = 'f00dfeedf00dfeedf00dfeedf00dfeed';
const skip = (process.platform === 'win32' || spawnSync('curl', ['--version']).status !== 0) && 'needs sh and curl';

/* runs a script, its app folder and service swapped for these, with bin first on the PATH */
function run(file, app, base, bin) {
  const text = fs.readFileSync(file, 'utf8').split(APP).join(app).split('http://127.0.0.1:47823').join(base);
  return new Promise((resolve) => {
    const env = Object.assign({}, process.env, { PATH: bin ? `${bin}${path.delimiter}${process.env.PATH}` : process.env.PATH });
    spawn('sh', ['-c', text], { env, stdio: 'ignore' }).on('close', resolve);
  });
}

for (const key of ['settings', 'input']) {
  test(`keys: ${key}-key.sh sends the token, and not on curl's command line`, { skip }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-keys-'));
    const svc = await mockServer(() => ({ json: { returnValue: true } }));
    try {
      fs.writeFileSync(path.join(dir, 'api-token'), TOKEN);
      const file = path.join(ROOT, 'service', `${key}-key.sh`);

      await run(file, dir, svc.base); /* the real curl: the service gets the key, with the token */
      assert.equal(svc.hits.length, 1);
      assert.equal(svc.hits[0].path, '/api/key');
      assert.equal(svc.hits[0].headers['x-homeplus-token'], TOKEN);
      assert.deepEqual(JSON.parse(svc.hits[0].body), { key });

      const bin = path.join(dir, 'bin'), seen = path.join(dir, 'argv'); /* a curl that notes what it was given */
      fs.mkdirSync(bin);
      fs.writeFileSync(path.join(bin, 'curl'), `#!/bin/sh\necho "$*" >"${seen}"\ncat >/dev/null\n`, { mode: 0o755 });
      await run(file, dir, svc.base, bin);
      assert.ok(fs.readFileSync(seen, 'utf8').includes('/api/key'));
      assert.ok(!fs.readFileSync(seen, 'utf8').includes(TOKEN), 'the token is not in its arguments');
    } finally {
      await svc.close();
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
    }
  });
}
