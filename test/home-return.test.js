/* Getting back to Home+: an app that quits brings Home+ at once (SAM would bring LG's home a second later, which
 * Home+ then replaced), and SAM bringing Home+ back tells the launcher, which only checks every 100 ms while
 * hidden. The service runs on the preview's stand-in webOS, whose subscriptions say what each test scripts. */
'use strict';

const { describe, test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { startService, sleep, until } = require('./helpers');

const HP = 'com.github.pbalazaar.homeplus';
/* Netflix in front, then no app, as on the C3 when an app quits: SAM says it closed, then that nothing's in front */
const IN_FRONT = [[1500, { appId: 'netflix' }], [2000, { appId: '' }]];
const CLOSED = [[1930, { event: 'close', appId: 'netflix' }], [1940, { event: 'stop', appId: 'netflix' }]];

/* the service, with subscriptions saying subs ({"<method>": [[ms, payload], ...]}, from when it subscribed) */
async function start(subs) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'homeplus-return-'));
  fs.writeFileSync(path.join(dir, 'apps.txt'), 'netflix|Netflix,youtube|YouTube');
  const luna = path.join(dir, 'luna.log');
  const svc = await startService(dir, {
    HOMEPLUS_PREVIEW_LUNA_LOG: luna,
    HOMEPLUS_PREVIEW_SUBSCRIPTIONS: JSON.stringify(subs),
    HOMEPLUS_LOG: path.join(dir, 'service.log'),
    HOMEPLUS_RECENTS_FILE: path.join(dir, 'recents.json')
  });
  const lines = () => (fs.existsSync(luna) ? fs.readFileSync(luna, 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l)) : []);
  return {
    svc,
    lines,
    launches: () => lines().filter((l) => l.uri === 'luna://com.webos.applicationManager/launch' && l.params && l.params.id === HP),
    said: (uri, appId) => lines().find((l) => l.uri.endsWith(uri) && l.said && l.said.appId === appId),
    log: () => fs.readFileSync(path.join(dir, 'service.log'), 'utf8'),
    async stop() {
      await svc.stop();
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
    }
  };
}

/* the launcher's open request for the service's events: resolves with them, or null after ms */
function events(svc, ms) {
  return new Promise((resolve) => {
    const req = http.get({ host: '127.0.0.1', port: svc.port, path: '/api/events', headers: { 'X-Homeplus-Token': svc.token } }, (res) => {
      let b = '';
      res.on('data', (c) => { b += c; });
      res.on('end', () => resolve(JSON.parse(b).events));
    });
    req.on('error', () => resolve(null));
    setTimeout(() => { req.destroy(); resolve(null); }, ms);
  });
}

describe('getting back to Home+', { concurrency: true }, () => {
  test('an app that quits: Home+ is launched at once, not after LG home', async () => {
    const t = await start({ getForegroundAppInfo: IN_FRONT, getAppLifeEvents: CLOSED });
    try {
      await until(() => t.launches().length, 4000, 'Home+ launched');
      const ms = t.launches()[0].t - t.said('getForegroundAppInfo', '').t;
      /* (45: the service's 50 ms timer can read a millisecond or two short here, as Node times it on libuv's
       * clock, not Date.now()'s; CI saw 49) */
      assert.ok(ms >= 45 && ms < 300, `launched ${ms} ms after the app quit (50 ms for another app to say it's coming)`);
      assert.match(t.log(), /home takeover \(netflix quit\) -> com\.github\.pbalazaar\.homeplus ok/);
    } finally {
      await t.stop();
    }
  });

  test('an app that quits, SAM saying so after the screen empties: Home+ is launched then', async () => {
    const t = await start({ getForegroundAppInfo: IN_FRONT, getAppLifeEvents: [[2300, { event: 'stop', appId: 'netflix' }]] });
    try {
      await until(() => t.launches().length, 4000, 'Home+ launched');
      const ms = t.launches()[0].t - t.said('getAppLifeEvents', 'netflix').t;
      assert.ok(ms >= 45 && ms < 300, `launched ${ms} ms after SAM said the app closed`); /* (45: as above) */
    } finally {
      await t.stop();
    }
  });

  test('an app sent to the background (LG brings back the app before it): left to LG', async () => {
    const t = await start({ getForegroundAppInfo: IN_FRONT, getAppLifeEvents: [[1950, { event: 'background', appId: 'netflix' }]] });
    try {
      await until(() => t.said('getForegroundAppInfo', ''), 4000, 'no app in front');
      await sleep(500);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('Home+ itself going (closed, or being removed): not launched again', async () => {
    const t = await start({ getForegroundAppInfo: [[1500, { appId: HP }], [2000, { appId: '' }]],
      getAppLifeEvents: [[1940, { event: 'stop', appId: HP }]] });
    try {
      await until(() => t.said('getForegroundAppInfo', ''), 4000, 'no app in front');
      await sleep(500);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('another app on its way to the front: left to it', async () => {
    /* (LG's compositor brings back the app before one that sets _WEBOS_LAUNCH_PREV_APP_AFTER_CLOSING) */
    const t = await start({ getForegroundAppInfo: IN_FRONT, getAppLifeEvents: [...CLOSED, [2020, { event: 'launch', appId: 'youtube' }]] });
    try {
      await until(() => t.said('getForegroundAppInfo', ''), 4000, 'no app in front');
      await sleep(500);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('the TV turning off: not launched', async () => {
    const t = await start({ getForegroundAppInfo: IN_FRONT, getAppLifeEvents: CLOSED,
      getPowerState: [[1000, { state: 'Active', processing: 'Request Power Off' }]] });
    try {
      await until(() => t.said('getForegroundAppInfo', ''), 4000, 'no app in front');
      await sleep(500);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('waking from standby opens Home+ when Live TV appears', async () => {
    const t = await start({
      getPowerState: [[100, { state: 'Active' }], [250, { state: 'Active Standby' }], [500, { state: 'Active' }]],
      getForegroundAppInfo: [[120, { appId: HP }], [600, { appId: 'com.webos.app.livetv' }]]
    });
    try {
      await until(() => t.launches().length, 2000, 'Home+ after wake');
      const live = t.said('getForegroundAppInfo', 'com.webos.app.livetv');
      assert.ok(t.launches()[0].t - live.t < 500, 'Home+ launched promptly after Live TV');
      await sleep(800);
      assert.equal(t.launches().length, 1);
      assert.match(t.log(), /power wake Active Standby -> Active/);
    } finally {
      await t.stop();
    }
  });

  test('waking to another app leaves that app in front', async () => {
    const t = await start({
      getPowerState: [[100, { state: 'Active' }], [250, { state: 'Active Standby' }], [500, { state: 'Active' }]],
      getForegroundAppInfo: [[600, { appId: 'netflix' }]]
    });
    try {
      await until(() => t.said('getForegroundAppInfo', 'netflix'), 2000, 'Netflix after wake');
      await sleep(800);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('Home Button off leaves Live TV in front after wake', async () => {
    const t = await start({
      getPowerState: [[100, { state: 'Active' }], [700, { state: 'Active Standby' }], [950, { state: 'Active' }]],
      getForegroundAppInfo: [[1050, { appId: 'com.webos.app.livetv' }]]
    });
    try {
      assert.equal((await t.svc.call('POST', '/api/home', { body: { mode: 'off' } })).json.mode, 'off');
      await until(() => t.said('getForegroundAppInfo', 'com.webos.app.livetv'), 2000, 'Live TV after wake');
      await sleep(800);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('Home+ not the home screen (Home Button off): not launched', async () => {
    const t = await start({ getForegroundAppInfo: [[3000, { appId: 'netflix' }], [3500, { appId: '' }]],
      getAppLifeEvents: [[3440, { event: 'stop', appId: 'netflix' }]] });
    try {
      assert.equal((await t.svc.call('POST', '/api/home', { body: { mode: 'off' } })).json.mode, 'off');
      await until(() => t.said('getForegroundAppInfo', ''), 6000, 'no app in front');
      await sleep(500);
      assert.equal(t.launches().length, 0);
    } finally {
      await t.stop();
    }
  });

  test('SAM bringing Home+ back from behind an app: the launcher hears it', async () => {
    const t = await start({ getForegroundAppInfo: [[1000, { appId: 'netflix' }]], getAppLifeEvents: [[2500, { event: 'launch', appId: HP }]] });
    try {
      await until(() => t.said('getForegroundAppInfo', 'netflix'), 4000, 'Netflix in front');
      assert.deepEqual(await events(t.svc, 4000), ['wake']);
    } finally {
      await t.stop();
    }
  });

  test('SAM launching Home+ while it is in front: nothing to tell', async () => {
    const t = await start({ getForegroundAppInfo: [[1000, { appId: HP }]], getAppLifeEvents: [[2500, { event: 'launch', appId: HP }]] });
    try {
      await until(() => t.said('getForegroundAppInfo', HP), 4000, 'Home+ in front');
      assert.equal(await events(t.svc, 2500), null);
    } finally {
      await t.stop();
    }
  });
});
