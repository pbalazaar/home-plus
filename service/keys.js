// The keys page: TMDB's key and Trakt's Client ID and Secret, pasted on a phone rather than typed with
// the remote. The TV shows a QR code (qr.js) for a link to this page, served on the local network
// while it's asked for: on a port of its own, at a path no one can guess (128 random bits), and only
// until the keys are saved or KEYS_TTL_MS goes by. The service's own API stays on 127.0.0.1.
'use strict';

const http = require('http');
const os = require('os');
const crypto = require('crypto');
const qr = require('./qr');

const TTL_MS = Number(process.env.HOMEPLUS_KEYS_TTL_MS) || 10 * 60 * 1000;

// the TV's address on the local network: Wi-Fi or Ethernet, IPv4
function lanAddress() {
  const all = os.networkInterfaces();
  const names = Object.keys(all).sort((a, b) => (/^(wlan|eth|en)/.test(b) ? 1 : 0) - (/^(wlan|eth|en)/.test(a) ? 1 : 0));
  for (const name of names) {
    for (const a of all[name] || []) {
      if ((a.family === 'IPv4' || a.family === 4) && !a.internal) return a.address;
    }
  }
  return '';
}

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function page({ tmdbSet, traktSet, notes = [], done = false }) {
  const note = notes.map((n) => `<p class="${n.ok ? 'ok' : 'err'}">${esc(n.text)}</p>`).join('');
  const form = done ? '<p>You can close this page.</p>' : `
<form method="post">
  <h2>TMDB <span>${tmdbSet ? 'set up' : 'not set up'}</span></h2>
  <p>What's popular on streaming apps. Make a free account at themoviedb.org, then under
  <a href="https://www.themoviedb.org/settings/api">Settings &rarr; API</a> copy the API key (or the API Read Access Token).</p>
  <label>API key or Read Access Token<input name="tmdbKey" autocomplete="off" autocapitalize="off" spellcheck="false"></label>
  <h2>Trakt <span>${traktSet ? 'set up' : 'not set up'}</span></h2>
  <p>Your Trakt progress in Continue Watching. Create an app at
  <a href="https://trakt.tv/oauth/applications/new">trakt.tv/oauth/applications/new</a> with
  <code>urn:ietf:wg:oauth:2.0:oob</code> as its redirect URI, then copy its Client ID and Client Secret.</p>
  <label>Client ID<input name="traktClientId" autocomplete="off" autocapitalize="off" spellcheck="false"></label>
  <label>Client Secret<input name="traktClientSecret" autocomplete="off" autocapitalize="off" spellcheck="false"></label>
  <p class="small">Leave a field empty to keep what's there.</p>
  <button>Save</button>
</form>`;
  return `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Home+ Keys</title><style>
body{font:16px/1.45 -apple-system,system-ui,sans-serif;background:#111114;color:#eee;margin:0;padding:24px 16px;max-width:560px;margin:auto}
h1{font-size:24px;margin:0 0 8px}h2{font-size:18px;margin:28px 0 4px}h2 span{font-size:13px;font-weight:400;color:#999;margin-left:6px}
a{color:#6cb4ff}p{color:#bbb;margin:6px 0;overflow-wrap:anywhere}code{color:#ddd;word-break:break-all}.small{font-size:13px;color:#888}
label{display:block;margin:12px 0;font-size:14px;color:#ccc}
input{display:block;width:100%;box-sizing:border-box;margin-top:4px;padding:12px;font:15px ui-monospace,monospace;border-radius:10px;border:1px solid #333;background:#1c1c20;color:#fff}
button{margin-top:20px;width:100%;padding:14px;font-size:17px;font-weight:600;border:0;border-radius:12px;background:#f5f5f7;color:#1c1c1e}
.ok{color:#5ad17a}.err{color:#ff6a5f}
</style></head><body><h1>Home+</h1>${note}${form}</body></html>`;
}

function readForm(req) {
  return new Promise((resolve) => {
    let buf = '';
    req.on('data', (c) => { if (buf.length < 8192) buf += c; });
    req.on('end', () => {
      const p = new URLSearchParams(buf), o = {};
      for (const k of ['tmdbKey', 'traktClientId', 'traktClientSecret']) o[k] = String(p.get(k) || '').trim();
      resolve(o);
    });
  });
}

module.exports = function createKeysPage({ tmdb, trakt, log, emit }) {
  let server = null, secret = '', url = '', timer = null;
  const sockets = new Set(); /* Node 16 (the TV's) has no closeAllConnections: a phone's kept-alive one goes too */

  function status() {
    return { open: !!server, url, tmdb: tmdb.status().configured, trakt: trakt.status().configured };
  }

  function close() {
    clearTimeout(timer);
    if (server) {
      server.close();
      for (const s of sockets) s.destroy();
      sockets.clear();
      log('keys page closed');
    }
    server = null;
    secret = url = '';
  }

  async function handle(req, res) {
    const send = (code, html) => {
      res.writeHead(code, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store', 'Referrer-Policy': 'no-referrer' });
      res.end(html);
    };
    const got = Buffer.from(String(req.url || '').split('?')[0]), want = Buffer.from(`/${secret}`);
    if (!secret || got.length !== want.length || !crypto.timingSafeEqual(got, want)) return send(404, 'Not found');
    const st = status();
    if (req.method !== 'POST') return send(200, page({ tmdbSet: st.tmdb, traktSet: st.trakt }));
    const f = await readForm(req), notes = [];
    let failed = false;
    if (f.tmdbKey) {
      try {
        await tmdb.setKey(f.tmdbKey);
        notes.push({ ok: true, text: 'TMDB key saved.' });
      } catch (e) {
        failed = true;
        notes.push({ ok: false, text: `TMDB: ${e.message}` });
      }
    }
    if (f.traktClientId || f.traktClientSecret) {
      if (!f.traktClientId || !f.traktClientSecret) {
        failed = true;
        notes.push({ ok: false, text: 'Trakt needs both the Client ID and the Client Secret.' });
      } else {
        trakt.setup(f.traktClientId, f.traktClientSecret);
        notes.push({ ok: true, text: 'Trakt saved. On the TV, sign in from Settings → Accounts → Trakt.' });
      }
    }
    if (!notes.length) notes.push({ ok: false, text: 'Nothing to save: paste a key first.' });
    const saved = notes.some((n) => n.ok);
    if (saved) {
      log('keys page: saved', notes.filter((n) => n.ok).length);
      emit('keys');
    }
    const done = saved && !failed;
    const now = status();
    send(200, page({ tmdbSet: now.tmdb, traktSet: now.trakt, notes, done }));
    if (done) setTimeout(close, 500); /* the link works once */
  }

  // opens the page (or keeps the one open), for TTL_MS from now: its link, and the QR code's
  function open() {
    return new Promise((resolve, reject) => {
      const host = process.env.HOMEPLUS_KEYS_HOST || lanAddress(); /* (tests: 127.0.0.1) */
      if (!host) return reject(new Error('The TV isn’t on a network'));
      clearTimeout(timer);
      timer = setTimeout(close, TTL_MS);
      if (server) return resolve(status());
      secret = crypto.randomBytes(16).toString('hex');
      server = http.createServer((req, res) => handle(req, res).catch((e) => { log('keys page error', e.message); res.end(); }));
      server.on('connection', (sock) => {
        sockets.add(sock);
        sock.on('close', () => sockets.delete(sock));
      });
      server.on('error', (e) => { log('keys page', e.message); close(); reject(e); });
      server.listen(Number(process.env.HOMEPLUS_KEYS_PORT) || 0, '0.0.0.0', () => {
        url = `http://${host}:${server.address().port}/${secret}`;
        log('keys page open on port', server.address().port);
        resolve(status());
      });
    });
  }

  function qrPng() {
    return url ? qr.png(url, 10) : null;
  }

  return { open, close, status, qrPng };
};
