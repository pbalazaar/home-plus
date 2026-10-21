/* Ambient light, for people who run HyperHDR and PicCap on the TV: two Control Center
 * switches, HyperHDR's LEDs and PicCap's screen capture. They're offered only when those
 * are installed. Nothing else in Home+ knows about them, but for Blur Over Video (Settings),
 * which pauses PicCap's capture (below): deleting this file and its line in service.js
 * removes them.
 */
'use strict';

const fs = require('fs');
const { requestJSON } = require('./net');
const tmp = require('./tmp');

const SERVICES = '/media/developer/apps/usr/palm/services';
// HyperHDR answers JSON-RPC on 127.0.0.1:8090 (no token from the TV itself); its loader service
// starts it. Tests point both elsewhere.
const HYPERHDR = process.env.HOMEPLUS_HYPERHDR || 'http://127.0.0.1:8090/json-rpc';
const HYPERHDR_DIR = process.env.HOMEPLUS_HYPERHDR_DIR || `${SERVICES}/org.webosbrew.hyperhdr.loader.service`;
const PICCAP_DIR = process.env.HOMEPLUS_PICCAP_DIR || `${SERVICES}/org.webosbrew.piccap.service`;
const LOADER = 'luna://org.webosbrew.hyperhdr.loader.service';
const PICCAP = 'luna://org.webosbrew.piccap.service';
const PAUSED = process.env.HOMEPLUS_PICCAP_PAUSED || '/tmp/homeplus-piccap-paused'; /* (gone at a restart, when PicCap starts anyway) */

module.exports = function createAmbient({ luna, log }) {
  const installed = (dir) => async () => fs.existsSync(dir);
  const rpc = async (cmd) => {
    const r = await requestJSON('POST', HYPERHDR, { body: Object.assign({ tan: 1 }, cmd), timeout: 2000 });
    if (!r.json || r.json.success === false) throw new Error(`HyperHDR: ${(r.json && r.json.error) || r.status}`);
    return r.json;
  };
  // HyperHDR's LEDs: its LEDDEVICE component; not running counts as off
  async function leds() {
    try {
      const c = ((await rpc({ command: 'serverinfo' })).info.components || []).find((x) => x.name === 'LEDDEVICE');
      return { running: true, on: !!(c && c.enabled) };
    } catch (e) {
      return { running: false, on: false };
    }
  }
  const capture = async () => {
    const r = await luna(`${PICCAP}/status`, {});
    if (!r || r.returnValue === false) throw new Error(`PicCap: ${(r && r.errorText) || "it didn't answer"}`);
    return !!r.isRunning;
  };
  const onOff = (on) => ({ on, value: on ? 'On' : 'Off' });

  // Blur Over Video: PicCap keeps the TV's one video capture while it captures, so the Control
  // Center over other apps gets no frames of the video to blur. With that setting on, it pauses
  // PicCap's capture while it's open (its switch still says On), and starts it again 2 s after, so
  // opening it again at once finds PicCap still paused, not starting. A mark in /tmp says it's
  // paused, in case the service stops in between: the next one starts it again.
  const AGAIN_MS = Number(process.env.HOMEPLUS_PICCAP_AGAIN_MS) || 2000;
  let paused = false, again = null;
  const setPaused = (on) => {
    paused = on;
    try { if (on) tmp.write(PAUSED, ''); else fs.rmSync(PAUSED, { force: true }); } catch (e) { /* /tmp (only ever its own file there) */ }
  };
  async function start(again) {
    const r = await luna(`${PICCAP}/start`, {});
    if (r && r.returnValue !== false) {
      log('ambient: capture started again');
    } else {
      log('ambient: capture didn\'t start again:', (r && r.errorText) || 'no answer');
      if (!again) setTimeout(() => start(true), 2000);
    }
  }
  if (tmp.has(PAUSED)) { /* paused by a service that stopped */
    setPaused(false);
    setImmediate(start);
  }

  return [
    {
      id: 'ambientLight', title: 'Ambient Light', short: 'Ambient', kind: 'switch', icon: 'ambient', group: 'Ambient Light', sizes: ['wide', 'small'],
      available: installed(HYPERHDR_DIR),
      read: async () => onOff((await leds()).on),
      run: async () => {
        const l = await leds();
        if (!l.running) { /* off with HyperHDR: start it, and its LEDs come on */
          const r = await luna(`${LOADER}/start`, {});
          if (!r || r.returnValue === false) throw new Error(`HyperHDR didn't start: ${(r && r.errorText) || 'no answer'}`);
          log('ambient: HyperHDR started');
          return;
        }
        await rpc({ command: 'componentstate', componentstate: { component: 'LEDDEVICE', state: !l.on } });
        log('ambient: LEDs', l.on ? 'off' : 'on');
      }
    },
    {
      id: 'screenCapture', title: 'Screen Capture', short: 'Capture', kind: 'switch', icon: 'capture', group: 'Ambient Light', sizes: ['wide', 'small'],
      available: installed(PICCAP_DIR),
      read: async () => onOff(paused || await capture()),
      run: async () => {
        if (paused) { /* turned off while the Control Center has it paused: it stays off */
          setPaused(false);
          log('ambient: capture stays stopped');
          return;
        }
        const on = await capture();
        const r = await luna(`${PICCAP}/${on ? 'stop' : 'start'}`, {});
        if (!r || r.returnValue === false) throw new Error(`PicCap: ${(r && r.errorText) || "it didn't answer"}`);
        log('ambient: capture', on ? 'stopped' : 'started');
      },
      // true if it was capturing, and now isn't
      pause: async () => {
        if (again) { /* opened again before PicCap was started: it stays paused */
          clearTimeout(again);
          again = null;
          return true;
        }
        if (paused || !fs.existsSync(PICCAP_DIR) || !(await capture().catch(() => false))) return false;
        const r = await luna(`${PICCAP}/stop`, {});
        if (!r || r.returnValue === false) return false;
        setPaused(true);
        log('ambient: capture paused, for the Control Center\'s glass');
        return true;
      },
      resume: () => {
        if (!paused || again) return; /* (or turned off in between) */
        again = setTimeout(() => {
          again = null;
          if (!paused) return;
          setPaused(false);
          start();
        }, AGAIN_MS);
      }
    }
  ];
};
