/* Stremio's streaming server is a webOS service of its own (io.strem.tv.server), and it keeps running
 * after Stremio closes: ~30 MB that webOS's memory manager never takes back, since it closes apps, not
 * services. Once Stremio has closed and stayed closed for a moment, this stops the server; Stremio
 * starts it again when it opens. Deleting this file and its lines in service.js removes it.
 */
'use strict';

const fs = require('fs');
const path = require('path');

const APP = 'io.strem.tv';
const SERVER = 'io.strem.tv.server';
const PROC = process.env.HOMEPLUS_PROC || '/proc';
// Stremio opened again within this long (a quick restart) keeps its server
const GRACE_MS = Number(process.env.HOMEPLUS_STREMIO_GRACE_MS) || 10000;

module.exports = function createStremioServer({ luna, log, kill = process.kill }) {
  let timer = null;

  /* the server's processes: Node with the service's id for its title (its command line) */
  function pids() {
    let names = [];
    try { names = fs.readdirSync(PROC); } catch (e) { return []; }
    return names.filter((name) => {
      if (!/^\d+$/.test(name)) return false;
      try {
        return fs.readFileSync(path.join(PROC, name, 'cmdline'), 'latin1').split('\0')[0].trim() === SERVER;
      } catch (e) {
        return false; /* (it ended meanwhile) */
      }
    }).map(Number);
  }

  /* is Stremio running, in front or not? null when SAM doesn't say */
  async function stremioRunning() {
    const r = await luna('luna://com.webos.applicationManager/running', {});
    if (!r || r.returnValue === false || !Array.isArray(r.running)) return null;
    return r.running.some((a) => a.id === APP);
  }

  async function stop(why) {
    timer = null;
    if ((await stremioRunning()) !== false) return;
    for (const pid of pids()) {
      try {
        kill(pid, 'SIGTERM');
        log(`stremio: stopped its server, pid ${pid} (${why})`);
      } catch (e) {
        log(`stremio: couldn't stop its server, pid ${pid}: ${e.message}`);
      }
    }
  }

  const later = (why) => {
    clearTimeout(timer);
    timer = setTimeout(() => stop(why).catch((e) => log('stremio: server check failed', e.message)), GRACE_MS);
  };

  return {
    /* an app life event (getAppLifeEvents): Stremio closed, or is on its way again */
    onLife(appId, event) {
      if (appId !== APP) return;
      clearTimeout(timer);
      timer = null;
      if (event === 'close' || event === 'stop') later('Stremio closed');
    },
    /* the service has just started: a server Stremio left before (an update restarts the service) */
    check() {
      later("Stremio isn't open");
    }
  };
};
