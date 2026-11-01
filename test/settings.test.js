/* Settings over other apps: the pages the service describes for the panel (service/settings.js), and what
 * each row changes. test/overlay.test.js has them over the API, on the real service. */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const createSettings = require('../service/settings');
const { configStore } = require('./helpers');

function setup({ config = {}, home = {}, apps = [], tv = [{ id: 'com.palm.app.settings', label: 'All Settings' }], accounts = {} } = {}) {
  const store = configStore(config), homeAsked = [];
  const setPrefs = (b) => {
    const cfg = store.readConfig();
    cfg.prefs = Object.assign({}, cfg.prefs, b);
    store.writeConfig(cfg);
    return cfg.prefs;
  };
  const settings = createSettings({
    log: () => {},
    readConfig: store.readConfig,
    setPrefs,
    home: async () => Object.assign({ mode: 'watch', noFlashActive: false, noFlashAvailable: true, settingsKey: { available: true, on: true },
      inputKey: { available: true, on: false }, piccap: true }, home),
    setHome: async (b) => { homeAsked.push(b); },
    apps: async () => apps,
    tvApps: async () => tv,
    accounts: () => Object.assign({ stremio: { connected: false }, trakt: { configured: false, connected: false }, jellyfin: { connected: false },
      plex: { connected: false }, tmdb: { configured: false } }, accounts),
    version: '1.2.3',
    elevated: () => true
  });
  const pages = async () => (await settings.state()).pages;
  const page = async (id) => (await pages()).find((p) => p.id === id);
  const row = async (pageId, id) => (await page(pageId)).rows.find((r) => r.id === id);
  return { settings, store, homeAsked, pages, page, row };
}

test('settings: the pages, in the launcher\'s order, and those this TV has a use for', async () => {
  assert.deepEqual((await setup().pages()).map((p) => p.id), ['remote', 'display', 'tiles', 'controlCenter', 'general', 'accessibility', 'accounts', 'tv', 'about']);
  const ids = async (o) => (await setup(o).pages()).map((p) => p.id);
  assert.ok(!(await ids({ home: { piccap: false } })).includes('controlCenter'), 'no PicCap: nothing to pause');
  assert.ok(!(await ids({ home: { settingsKey: { available: false, on: false } } })).includes('controlCenter'), 'not over other apps: no Control Center page');
  assert.ok(!(await ids({ tv: [] })).includes('tv'), 'none of LG\'s settings apps');
});

test('settings: a row says what it is, and how it changes stays in the service', async () => {
  const pages = JSON.parse(JSON.stringify(await setup().pages()));
  for (const r of [].concat(...pages.map((p) => p.rows))) {
    assert.ok(r.id && r.kind && r.label, `a row with an id, a kind and a label: ${JSON.stringify(r)}`);
    assert.ok(!('set' in r));
  }
});

test('settings: a time steps through the launcher\'s choices, both ways, and goes round', async () => {
  const { settings, store, row } = setup({ config: { prefs: { dimMinutes: 10 } } });
  assert.equal((await row('display', 'dimAfter')).value, '10 min');
  await settings.act('dimAfter', 1);
  assert.equal(store.get().prefs.dimMinutes, -1);
  assert.equal((await row('display', 'dimAfter')).value, 'Never');
  await settings.act('dimAfter', 1);
  assert.equal(store.get().prefs.dimMinutes, 1, 'round to the first');
  await settings.act('dimAfter', -1);
  assert.equal(store.get().prefs.dimMinutes, -1, 'and back');
});

test('settings: with nothing saved, the times are the launcher\'s defaults, and step from them', async () => {
  const { settings, store, row } = setup();
  assert.equal((await row('display', 'dimAfter')).value, '2 min');
  assert.equal((await row('display', 'offAfter')).value, '10 min');
  await settings.act('dimAfter', 1);
  await settings.act('offAfter', -1);
  assert.equal(store.get().prefs.dimMinutes, 5);
  assert.equal(store.get().prefs.offMinutes, 5);
});

test('settings: what\'s on by default says so, and turns off', async () => {
  const { settings, store, row } = setup();
  for (const [id, page, key] of [['pixelShift', 'display', 'pixelShift'], ['calmColours', 'display', 'calmColours']]) {
    assert.equal((await row(page, id)).value, 'On');
    await settings.act(id, 1);
    assert.equal(store.get().prefs[key], false);
    assert.equal((await row(page, id)).value, 'Off');
  }
  assert.equal((await row('general', 'sounds')).value, 'On');
  await settings.act('sounds', -1);
  assert.equal(store.get().prefs.sounds, false, 'Left turns it too');
  assert.equal((await row('display', 'topShelf')).value, 'Turns Pages');
  await settings.act('topShelf', 1);
  assert.equal(store.get().prefs.shelfTurns, false);
  assert.equal((await row('display', 'topShelf')).value, 'Stays Still');
  assert.equal((await row('general', 'clock')).value, '12-Hour');
  await settings.act('clock', 1);
  assert.equal((await row('general', 'clock')).value, '24-Hour');
  for (const id of ['reduceMotion', 'reduceTransparency']) {
    assert.equal((await row('accessibility', id)).value, 'Off');
    await settings.act(id, 1);
    assert.equal((await row('accessibility', id)).value, 'On');
  }
});

test('settings: Top Shelf Details goes off, compact, full, both ways', async () => {
  const { settings, store, row } = setup();
  assert.equal((await row('display', 'topShelfDetails')).value, 'Full');
  await settings.act('topShelfDetails', 1);
  assert.equal(store.get().prefs.topShelfDetails, 'off');
  await settings.act('topShelfDetails', -1);
  assert.equal(store.get().prefs.topShelfDetails, 'full');
  await settings.act('topShelfDetails', -1);
  assert.equal((await row('display', 'topShelfDetails')).value, 'Compact');
});

test('settings: the Remote page asks for what the launcher\'s buttons ask for', async () => {
  const on = setup();
  await on.settings.act('homeButton', 1);
  await on.settings.act('noFlash', 1);
  await on.settings.act('settingsButton', 1);
  await on.settings.act('inputButton', 1);
  assert.deepEqual(on.homeAsked, [{ mode: 'off', asked: true }, { mode: 'redirect' }, { settingsKey: false }, { inputKey: true }]);

  const off = setup({ home: { mode: 'off' } });
  assert.equal((await off.row('remote', 'homeButton')).value, 'Opens LG Home');
  assert.equal(await off.row('remote', 'noFlash'), undefined, 'No Flash is for a Home button that\'s Home+\'s');
  await off.settings.act('homeButton', 1);
  assert.deepEqual(off.homeAsked, [{ mode: 'redirect', asked: true }], 'on, with No Flash where the TV has it');

  const plain = setup({ home: { mode: 'off', noFlashAvailable: false } });
  await plain.settings.act('homeButton', 1);
  assert.deepEqual(plain.homeAsked, [{ mode: 'watch', asked: true }], 'and without it where it doesn\'t');
});

test('settings: on a TV where nothing catches the keys, the Remote page says what would', async () => {
  const { page } = setup({ home: { noFlashAvailable: false, settingsKey: { available: false, on: false } } });
  const remote = await page('remote');
  assert.deepEqual(remote.rows.map((r) => r.id), ['homeButton']);
  assert.match(remote.body, /LG Input Hook/);
});

test('settings: App Tiles ticks its look, and Tinted brings its colours', async () => {
  const { settings, store, page, row } = setup();
  assert.deepEqual((await page('tiles')).rows.map((r) => [r.label, r.on]), [['Default', true], ['Dark', false], ['Clear', false], ['Tinted', false]]);
  await settings.act('look-tinted');
  assert.equal(store.get().prefs.iconLook, 'tinted');
  const tint = await row('tiles', 'tint');
  assert.equal(tint.kind, 'tint');
  assert.equal(tint.label, 'Indigo', 'the launcher\'s default');
  assert.equal(tint.colours[tint.on], '#5e5ce6');
  await settings.act('look-dark');
  assert.equal(await row('tiles', 'tint'), undefined, 'gone with Tinted');
});

test('settings: Left and Right stop at the last colour, and OK goes round', async () => {
  const { settings, store, row } = setup({ config: { prefs: { iconLook: 'tinted', iconTint: 'Red' } } });
  await settings.act('tint', -1);
  assert.equal(store.get().prefs.iconTint, 'Red', 'Left on the first');
  await settings.act('tint', 1);
  assert.equal(store.get().prefs.iconTint, 'Orange');
  await settings.act('tint', 'next');
  assert.equal((await row('tiles', 'tint')).label, 'Yellow');
  store.writeConfig({ prefs: { iconLook: 'tinted', iconTint: 'Graphite' } });
  await settings.act('tint', 1);
  assert.equal(store.get().prefs.iconTint, 'Graphite', 'Right on the last');
  await settings.act('tint', 'next');
  assert.equal(store.get().prefs.iconTint, 'Red', 'OK on the last');
});

test('settings: Hidden Apps shows how many, and opens in Home+', async () => {
  assert.equal(await setup().row('general', 'hidden'), undefined, 'none hidden');
  const r = await setup({ config: { layout: { order: [], hidden: ['a', 'b'] } } }).row('general', 'hidden');
  assert.equal(r.value, '2');
  assert.deepEqual([r.kind, r.page], ['home', 'general']);
});

test('settings: Accounts lists the sign-ins this TV has apps for, and each opens in Home+', async () => {
  const ids = async (o) => (await setup(o).page('accounts')).rows.map((r) => r.id);
  assert.deepEqual(await ids({}), ['trakt', 'tmdb'], 'no apps: what needs none');
  assert.deepEqual(await ids({ apps: [{ id: 'io.strem.tv', title: 'Stremio' }, { id: 'org.moonfin.webos', title: 'Moonfin' }, { id: 'cdp-30', title: 'Plex' }] }),
    ['stremio', 'trakt', 'jellyfin', 'plex', 'tmdb'], 'by name, by id, and the other clients the launcher knows');
  assert.deepEqual(await ids({ accounts: { jellyfin: { connected: true } } }), ['trakt', 'jellyfin', 'tmdb'], 'signed in, it stays with its app gone');

  const { page } = setup({ apps: [{ id: 'io.strem.tv', title: 'Stremio' }], accounts: { stremio: { connected: true }, trakt: { configured: true, connected: false }, tmdb: { configured: true } } });
  const accounts = await page('accounts');
  assert.deepEqual(accounts.rows.map((r) => r.value), ['Signed In', 'Not Connected', 'Set Up']);
  assert.ok(accounts.rows.every((r) => r.kind === 'home' && r.page === 'accounts'));
  assert.ok(accounts.body, 'with its words');
});

test('settings: webOS\'s long list of apps isn\'t asked for at each key press', async () => {
  let asked = 0;
  const settings = createSettings({
    log: () => {}, readConfig: () => ({}), setPrefs: () => {}, setHome: async () => {},
    home: async () => ({ mode: 'watch', noFlashAvailable: false, settingsKey: { available: false }, inputKey: {}, piccap: false }),
    apps: async () => { asked++; return []; }, tvApps: async () => [],
    accounts: () => ({ stremio: {}, trakt: {}, jellyfin: {}, plex: {}, tmdb: {} }), version: '1', elevated: () => true
  });
  await settings.state();
  await settings.act('clock', 1);
  await settings.state();
  assert.equal(asked, 1);
});

test('settings: TV Settings opens LG\'s apps', async () => {
  const [r] = (await setup().page('tv')).rows;
  assert.deepEqual([r.kind, r.app, r.label], ['app', 'com.palm.app.settings', 'All Settings']);
});

test('settings: About says what the launcher says, and Remove Home+ opens in Home+', async () => {
  const about = await setup().page('about');
  assert.match(about.body, /^Home\+ 1\.2\.3, set up with Homebrew Channel\n/);
  assert.deepEqual(about.rows.map((r) => [r.kind, r.page, r.destructive]), [['home', 'about', true]]);
});

test('settings: only what\'s offered can be changed', async () => {
  const { settings } = setup();
  await assert.rejects(settings.act('nothing', 1), /not a setting/);
  await assert.rejects(settings.act('remove', 1), /not a setting/, 'what opens Home+ changes nothing here');
  await assert.rejects(settings.act('tv-com.palm.app.settings', 1), /not a setting/);
});
