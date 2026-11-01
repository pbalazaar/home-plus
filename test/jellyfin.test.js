'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const dgram = require('dgram');
const http = require('http');
const { EventEmitter } = require('events');
const { mockServer, configStore, quiet, last, sleep, until } = require('./helpers');
const createJellyfin = require('../service/jellyfin');

const MIN = 600000000; /* Jellyfin times are 100 ns ticks: one minute */
const EPISODE = {
  Id: 'e1', Type: 'Episode', Name: 'Pilot', SeriesName: 'Show', ParentIndexNumber: 1, IndexNumber: 2,
  RunTimeTicks: 50 * MIN, UserData: { PlaybackPositionTicks: 20 * MIN, LastPlayedDate: '2026-10-20T10:00:00Z' },
  ImageTags: { Primary: 'p1' }, ParentBackdropItemId: 'show1', ParentBackdropImageTags: ['bk1'],
  ParentLogoItemId: 'show1', ParentLogoImageTag: 'lg1'
};
const MOVIE = {
  Id: 'm1', Type: 'Movie', Name: 'Film', RunTimeTicks: 100 * MIN, UserData: { PlaybackPositionTicks: 25 * MIN },
  ImageTags: { Logo: 'lg2', Thumb: 'th2' }, BackdropImageTags: ['bk2']
};
const NEXT = { Id: 'e2', Type: 'Episode', Name: 'Next', SeriesName: 'Show', ParentIndexNumber: 1, IndexNumber: 3,
  RunTimeTicks: 50 * MIN, UserData: { PlaybackPositionTicks: 0 }, ImageTags: { Primary: 'p2' } };

/* A stand-in Jellyfin server; mode changes how it behaves. */
let server, mode;
test.before(async () => {
  server = await mockServer((req, url) => {
    const signedIn = /Token="tok"/.test(req.headers.authorization || '');
    switch (url.pathname) {
      case '/QuickConnect/Initiate':
        return mode.quickConnectOff ? { status: 401, json: {} } : { json: { Code: '123456', Secret: 's1' } };
      case '/QuickConnect/Connect':
        return { json: { Authenticated: !!mode.approved } };
      case '/Users/AuthenticateWithQuickConnect':
        return { json: { AccessToken: 'tok', User: { Id: 'u1', Name: 'Ann' } } };
      case '/Sessions/Logout':
        return { status: 204, body: '' };
    }
    if (mode.down) return { status: 500, json: {} };
    if (mode.expired || !signedIn) return { status: 401, json: {} };
    if (url.pathname === '/UserItems/Resume') return mode.oldServer ? { status: 404, json: {} } : { json: { Items: mode.resume || [EPISODE, MOVIE] } };
    if (url.pathname === '/Users/u1/Items/Resume') return { json: { Items: [EPISODE, MOVIE] } };
    if (url.pathname === '/Shows/NextUp') return { json: { Items: [EPISODE, NEXT] } };
    if (mode.silent) return { hang: true };
    if (url.pathname === '/Sessions/Capabilities') return { status: 204, body: '' };
    if (url.pathname === '/Sessions') return { json: mode.sessions() };
    if (/^\/Sessions\/[^/]+\/Playing$/.test(url.pathname)) return { status: mode.playRefused ? 400 : 204, body: '' };
    return null;
  });
});
test.after(() => server.close());
test.beforeEach(() => {
  mode = { sessions: () => [] };
  server.hits.length = 0;
});

const signedIn = () => ({ jellyfin: { url: server.base, name: 'Home', userId: 'u1', user: 'Ann', token: 'tok' }, jellyfinDevice: 'dev1' });
function makeJellyfin(cfg) {
  const store = configStore(cfg);
  return { jf: createJellyfin({ ...store, log: quiet, version: 'test' }), store };
}

test('jellyfin: what you\'re partway through, then the next episodes, without repeats', async () => {
  const { jf } = makeJellyfin(signedIn());
  const items = await jf.items();
  assert.deepEqual(items.map((it) => it.id), ['e1', 'm1', 'e2']);
  const [ep, movie] = items;
  assert.equal(ep.type, 'series');
  assert.equal(ep.name, 'Show');
  assert.equal(ep.season, 1);
  assert.equal(ep.episode, 2);
  assert.equal(ep.progress, 0.4);
  assert.equal(ep.remainingMin, 30);
  assert.equal(ep.background, `${server.base}/Items/show1/Images/Backdrop?maxWidth=1920&quality=85&tag=bk1&api_key=tok`);
  assert.equal(ep.logo, `${server.base}/Items/show1/Images/Logo?maxWidth=500&quality=85&tag=lg1&api_key=tok`, 'the series\' logo');
  assert.equal(movie.type, 'movie');
  assert.equal(movie.progress, 0.25);
  assert.equal(movie.remainingMin, 75);
  assert.equal(movie.logo, `${server.base}/Items/m1/Images/Logo?maxWidth=500&quality=85&tag=lg2&api_key=tok`);
  assert.equal(movie.thumbnail, `${server.base}/Items/m1/Images/Thumb?maxWidth=480&quality=85&tag=th2&api_key=tok`);
});

test('jellyfin: found on the network, a server answering from this TV, or not as a web server, isn\'t taken', async (t) => {
  t.mock.method(dgram, 'createSocket', () => {
    const sock = new EventEmitter();
    sock.bind = (cb) => setImmediate(cb);
    sock.setBroadcast = () => {};
    sock.close = () => {};
    sock.send = (msg, port, address) => {
      assert.deepEqual([String(msg), port, address], ['who is JellyfinServer?', 7359, '255.255.255.255']);
      for (const answer of ['not JSON', { Address: 'http://127.0.0.1:8096', Name: 'This TV' }, { Address: 'http://[::ffff:127.0.0.1]:8096' },
        { Address: 'ftp://192.168.1.21' }, { Id: 'a', Address: 'http://192.168.1.20:8096/', Name: 'Den' }]) {
        setImmediate(() => sock.emit('message', Buffer.from(typeof answer === 'string' ? answer : JSON.stringify(answer))));
      }
    };
    return sock;
  });
  const asked = [];
  t.mock.method(http, 'request', (options) => { /* where Quick Connect is asked for: no network here */
    asked.push(options);
    const req = new EventEmitter();
    req.end = () => setImmediate(() => req.emit('error', new Error('no network here')));
    req.destroy = () => {};
    return req;
  });
  const { jf } = makeJellyfin({});
  await assert.rejects(jf.startLink(), /no network here/);
  assert.deepEqual(asked.map((o) => [o.hostname, o.port, o.path, o.method]), [['192.168.1.20', '8096', '/QuickConnect/Initiate', 'POST']]);
});

test('jellyfin: older servers (and Emby) use the per-user resume route', async () => {
  mode.oldServer = true;
  const { jf } = makeJellyfin(signedIn());
  assert.equal((await jf.items()).length, 3);
  assert.ok(server.hits.some((h) => h.path === '/Users/u1/Items/Resume'));
});

test('jellyfin: a rejected token signs out', async () => {
  mode.expired = true;
  const { jf, store } = makeJellyfin(signedIn());
  assert.deepEqual(await jf.items(), []);
  assert.equal(store.get().jellyfin, undefined);
  assert.equal(jf.status().connected, false);
});

test('jellyfin: a failed refresh keeps the last list', async (t) => {
  const { jf } = makeJellyfin(signedIn());
  const first = await jf.items();
  const now = Date.now();
  t.mock.method(Date, 'now', () => now + 61 * 1000); /* past the minute the list is kept */
  mode.down = true;
  assert.deepEqual(await jf.items(), first);
});

test('jellyfin: Quick Connect turned off says where to turn it on', async () => {
  mode.quickConnectOff = true;
  const { jf } = makeJellyfin({ jellyfinServer: server.base });
  const st = await jf.startLink();
  assert.equal(st.pending, null);
  assert.match(st.error, /Dashboard/);
});

test('jellyfin: sign-in with Quick Connect, approved on another device', async () => {
  const { jf, store } = makeJellyfin({ jellyfinServer: server.base });
  const st = await jf.startLink();
  assert.equal(st.pending.code, '123456');
  mode.approved = true;
  await until(() => jf.status().connected, 8000, 'the sign-in');
  assert.deepEqual(store.get().jellyfin, { url: server.base, name: 'Jellyfin', userId: 'u1', user: 'Ann', token: 'tok' });
});

test('jellyfin: signing out ends the session on the server too', async () => {
  const { jf, store } = makeJellyfin(signedIn());
  await jf.disconnect();
  const logout = last(server.hits.filter((h) => h.path === '/Sessions/Logout'));
  assert.ok(logout, 'the server was told');
  assert.match(logout.headers.authorization, /Token="tok"/);
  assert.equal(store.get().jellyfin, undefined);
});

test('jellyfin: a server address must be http or https', () => {
  const { jf, store } = makeJellyfin({});
  assert.throws(() => jf.setServer('ftp://host'), /http/);
  jf.setServer('http://host:8096/');
  assert.equal(store.get().jellyfinServer, 'http://host:8096');
  jf.setServer('');
  assert.equal(store.get().jellyfinServer, undefined);
});

/* OK on a title: as the C3 and the test server showed them, Home+'s entry (once it has registered) and the TV's app */
const AT = '192.168.1.50';
const HOME = { Id: 'home', DeviceId: 'dev1', DeviceName: 'LG TV', Client: 'Home+', RemoteEndPoint: AT, SupportsRemoteControl: false };
const app = (more) => Object.assign({ Id: 'tv', DeviceId: 'TW96aWxsYQ1', DeviceName: 'LG Smart TV', Client: 'Jellyfin Web', RemoteEndPoint: AT, SupportsRemoteControl: true }, more);
const FAST = { sessionMs: 150, startMs: 150, pollMs: 10 }; /* (25 s, 15 s and 1 s on the TV) */
const wanted = () => true;
const sent = () => server.hits.filter((h) => /\/Playing$/.test(h.path));
const asked = (p) => server.hits.filter((h) => h.path === p);
/* the server's list: this before PlayNow, and from then on this and whatever the app reports */
function listing(before, after = []) {
  mode.sessions = () => (sent().length ? after : before);
}

test('jellyfin: OK plays the title on this TV, from where it stopped', async () => {
  listing([HOME, app()], [HOME, app({ NowPlayingItem: { Id: 'm1' } })]);
  const { jf } = makeJellyfin(signedIn());
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), true);
  assert.deepEqual(sent().map((h) => h.path), ['/Sessions/tv/Playing']);
  const [play] = sent();
  assert.equal(play.method, 'POST');
  assert.equal(play.query.get('playCommand'), 'PlayNow');
  assert.equal(play.query.get('itemIds'), 'm1');
  assert.equal(play.query.get('startPositionTicks'), String(25 * MIN));
  assert.match(play.headers.authorization, /Token="tok"/);
  const paths = server.hits.map((h) => h.path);
  assert.ok(paths.indexOf('/Sessions/Capabilities') >= 0 && paths.indexOf('/Sessions/Capabilities') < paths.indexOf('/Sessions'), 'it registers before it reads the list');
  assert.equal(asked('/Sessions/Capabilities')[0].method, 'POST');
  assert.equal(asked('/Sessions/Capabilities').length, 1);
});

test('jellyfin: From Beginning and a next episode start at 0', async () => {
  for (const item of [{ id: 'm1', fromStart: true }, { id: 'e2' }]) {
    server.hits.length = 0;
    listing([HOME, app()], [HOME, app({ NowPlayingItem: { Id: item.id } })]);
    const { jf } = makeJellyfin(signedIn());
    assert.equal(await jf.playOnTV(item, wanted, FAST), true, item.id);
    assert.equal(sent()[0].query.get('startPositionTicks'), '0', item.id);
  }
});

test('jellyfin: the position is the server\'s as of now, not the list\'s', async () => {
  listing([HOME, app()], [HOME, app({ NowPlayingItem: { Id: 'm1' } })]);
  const { jf } = makeJellyfin(signedIn());
  await jf.items();
  mode.resume = [Object.assign({}, MOVIE, { UserData: { PlaybackPositionTicks: 40 * MIN } })];
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), true);
  assert.equal(sent()[0].query.get('startPositionTicks'), String(40 * MIN));
});

test('jellyfin: the TV\'s session is found under either of its client names', async () => {
  for (const Client of ['Jellyfin for WebOS', 'Jellyfin Web']) {
    server.hits.length = 0;
    listing([HOME, app({ Client })], [HOME, app({ Client, NowPlayingItem: { Id: 'm1' } })]);
    const { jf } = makeJellyfin(signedIn());
    assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), true, Client);
  }
});

test('jellyfin: which session is this TV\'s', async (t) => {
  const elsewhere = { RemoteEndPoint: '192.168.1.51' };
  const cases = [
    ['another TV, at another address', [HOME, app(elsewhere)], null],
    ['another kind of client', [HOME, app({ DeviceName: 'Chrome', Client: 'Jellyfin Web' })], null],
    ['a closed socket', [HOME, app({ SupportsRemoteControl: false })], null],
    ['two at this address: it picks neither', [HOME, app({ Id: 'a' }), app({ Id: 'b', DeviceId: 'other' })], null],
    ['Home+\'s own entry missing', [app()], null],
    ['neither has an address yet (just signed in)', [Object.assign({}, HOME, { RemoteEndPoint: null }), app({ RemoteEndPoint: null })], null],
    ['one here, one elsewhere', [HOME, app({ Id: 'there', ...elsewhere }), app({ Id: 'here' })], '/Sessions/here/Playing'],
    ['the session its playback is reported on can\'t be sent commands', [HOME, app(), app({ Id: 'reporting', DeviceId: 'b', SupportsRemoteControl: false })], '/Sessions/tv/Playing']
  ];
  for (const [what, list, path] of cases) {
    await t.test(what, async () => {
      server.hits.length = 0;
      listing(list, list);
      const { jf } = makeJellyfin(signedIn());
      assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), false, 'nothing reports it playing here');
      assert.deepEqual(sent().map((h) => h.path), path ? [path] : []);
    });
  }
});

test('jellyfin: it waits for the app to connect, then sends PlayNow once', async () => {
  let reads = 0;
  mode.sessions = () => (sent().length ? [HOME, app({ NowPlayingItem: { Id: 'm1' } })] : ++reads > 3 ? [HOME, app()] : [HOME]);
  const { jf } = makeJellyfin(signedIn());
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), true);
  assert.ok(reads > 3);
  assert.equal(sent().length, 1);
});

test('jellyfin: a title that starts on another of this TV\'s sessions has started', async () => {
  const reporting = app({ Id: 'reporting', DeviceId: 'second', SupportsRemoteControl: false, NowPlayingItem: { Id: 'm1' } });
  listing([HOME, app()], [HOME, app(), reporting]);
  const { jf } = makeJellyfin(signedIn());
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), true);
  assert.deepEqual(sent().map((h) => h.path), ['/Sessions/tv/Playing'], 'PlayNow went to the one that can take it');
});

test('jellyfin: another TV playing the title, or this one playing another title, isn\'t it starting', async () => {
  for (const other of [app({ Id: 'x', RemoteEndPoint: '192.168.1.51', NowPlayingItem: { Id: 'm1' } }), app({ Id: 'x', SupportsRemoteControl: false, NowPlayingItem: { Id: 'e1' } })]) {
    server.hits.length = 0;
    listing([HOME, app()], [HOME, app(), other]);
    const { jf } = makeJellyfin(signedIn());
    assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), false);
    assert.equal(sent().length, 1, 'PlayNow isn\'t sent again');
  }
});

test('jellyfin: it stops as soon as the title is no longer wanted, and sends nothing after', async () => {
  listing([HOME]);
  const { jf } = makeJellyfin(signedIn());
  let wants = true;
  setTimeout(() => {
    wants = false;
    listing([HOME, app()]); /* (the app connects too late) */
  }, 40);
  assert.equal(await jf.playOnTV({ id: 'm1' }, () => wants, { sessionMs: 5000, startMs: 150, pollMs: 10 }), false);
  await sleep(30);
  assert.equal(sent().length, 0);
});

test('jellyfin: no longer wanted while the title starts, it stops watching', async () => {
  listing([HOME, app()], [HOME, app()]);
  const { jf } = makeJellyfin(signedIn());
  let wants = true;
  const run = jf.playOnTV({ id: 'm1' }, () => wants, { sessionMs: 150, startMs: 5000, pollMs: 10 });
  await until(() => sent().length, 2000, 'PlayNow');
  wants = false;
  const stopped = Date.now();
  assert.equal(await run, false);
  assert.ok(Date.now() - stopped < 1000);
});

test('jellyfin: a title that has left the list isn\'t played', async () => {
  listing([HOME, app()]);
  const { jf } = makeJellyfin(signedIn());
  assert.equal(await jf.playOnTV({ id: 'gone' }, wanted, FAST), false);
  assert.equal(sent().length, 0);
});

test('jellyfin: a server that takes the call and never answers (it\'s asleep) is given up on soon', async () => {
  const { jf } = makeJellyfin(signedIn());
  await jf.items();
  mode.silent = true;
  const began = Date.now();
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, { sessionMs: 5000, startMs: 5000, pollMs: 10, registerMs: 100 }), false);
  assert.ok(Date.now() - began < 1000, 'not the 25 s a session gets');
  assert.equal(asked('/Sessions').length, 0);
});

test('jellyfin: OK knows a title from the list as it stands, however old, without asking the server', async (t) => {
  const { jf } = makeJellyfin(signedIn());
  await jf.items();
  const now = Date.now(), before = server.hits.length;
  t.mock.method(Date, 'now', () => now + 61 * 1000); /* past the minute the list is kept */
  mode.silent = true;
  assert.equal((await jf.known('m1')).name, 'Film');
  assert.equal(await jf.known('gone'), undefined);
  assert.equal(server.hits.length, before);
});

test('jellyfin: with no list yet, OK asks the server for one', async () => {
  const { jf } = makeJellyfin(signedIn());
  assert.equal((await jf.known('m1')).name, 'Film');
});

test('jellyfin: a server that can\'t be reached is given up on right away', async () => {
  const { jf } = makeJellyfin(signedIn());
  await jf.items();
  mode.down = true; /* (the list stays, as for the shelf: the next call says so) */
  const slow = { sessionMs: 5000, startMs: 5000, pollMs: 10 }, began = Date.now();
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, slow), false);
  assert.ok(Date.now() - began < 1000, 'it doesn\'t wait for the session');
  const nothing = makeJellyfin(signedIn()).jf; /* no list at all */
  assert.equal(await nothing.playOnTV({ id: 'm1' }, wanted, slow), false);
});

test('jellyfin: PlayNow refused: given up on right away', async () => {
  mode.playRefused = true;
  listing([HOME, app()]);
  const { jf } = makeJellyfin(signedIn());
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, { sessionMs: 150, startMs: 5000, pollMs: 10 }), false);
});

test('jellyfin: a rejected token signs out', async () => {
  const { jf, store } = makeJellyfin(signedIn());
  await jf.items();
  mode.expired = true;
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), false);
  assert.equal(store.get().jellyfin, undefined);
});

test('jellyfin: playing a title logs each step, without tokens', async () => {
  listing([HOME, app()], [HOME, app({ NowPlayingItem: { Id: 'm1' } })]);
  const lines = [];
  const store = configStore(signedIn());
  const jf = createJellyfin({ ...store, log: (...a) => lines.push(a.join(' ')), version: 'test' });
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), true);
  mode.down = true;
  assert.equal(await jf.playOnTV({ id: 'm1' }, wanted, FAST), false);
  assert.ok(lines.some((l) => /^jellyfin: resume m1 .*PlayNow/.test(l)), lines.join('\n'));
  assert.ok(lines.some((l) => /^jellyfin: resume m1 .*playing on/.test(l)), lines.join('\n'));
  assert.ok(!lines.some((l) => /tok/.test(l.replace(/token/g, ''))), lines.join('\n'));
});
