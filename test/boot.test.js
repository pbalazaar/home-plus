'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs'), os = require('os'), path = require('path');
const { execFileSync } = require('child_process');
const remove = require('../service/remove');
const quote = (s) => "'" + s.replace(/'/g, "'\\''") + "'";

for (const ready of [true, false]) {
  test('boot readiness uses a terminal and only starts Home+ after a successful ping: ' + ready,
    { skip: process.platform !== 'linux' }, () => {
      const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus boot-'));
      try {
        const bin = path.join(dir, 'bin'), svc = path.join(dir, 'service'), calls = path.join(dir, 'calls');
        fs.mkdirSync(bin); fs.mkdirSync(svc);
        fs.writeFileSync(path.join(svc, 'service.js'), '');
        fs.writeFileSync(path.join(bin, 'sleep'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
        fs.writeFileSync(path.join(bin, 'luna-send'), '#!/bin/sh\n' +
          'printf "%s\\n" "$*" >> "$CALLS"\n' +
          '[ -t 1 ] || exit 0\n' +
          'printf "%s\\n" "$REPLY"\n', { mode: 0o755 });
        const script = path.join(dir, 'boot.sh'), log = path.join(dir, 'boot.log');
        const source = fs.readFileSync(path.join(__dirname, '../init.d/50-homeplus'), 'utf8')
          .replace(/^SVC_DIR=.*$/m, () => 'SVC_DIR=' + quote(svc))
          .replace(/^ELEVATE=.*$/m, () => 'ELEVATE=' + quote(path.join(dir, 'absent')))
          .replace(/^LOG=.*$/m, () => 'LOG=' + quote(log));
        fs.writeFileSync(script, source);
        execFileSync('/bin/sh', ['-c', '. "$1"; wait', 'homeplus-boot', script], {
          env: Object.assign({}, process.env, {
            PATH: bin + path.delimiter + process.env.PATH, CALLS: calls, REPLY: JSON.stringify({ returnValue: ready })
          }), timeout: 10000
        });
        const lines = fs.readFileSync(calls, 'utf8').trim().split('\n');
        assert.equal(lines.filter((l) => l.includes('/startup ')).length, ready ? 1 : 0);
        assert.equal(lines.filter((l) => l.includes('/ping ')).length, ready ? 1 : 30);
      } finally { remove.rmSync(dir, { recursive: true, force: true }); }
    });
}
