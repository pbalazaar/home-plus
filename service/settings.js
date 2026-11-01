/* Settings over other apps: the launcher's Settings pages (native/src/settings.c) as data, for the Control
 * Center that opens over what's playing (native/qml/SettingsPanel.qml only draws them). The pages, rows and
 * words are the launcher's: test/qml-sizes.test.js keeps them the same. A row's set() is left out of the
 * answer, so all the panel can do is ask for a row by its id. What can't be done over an app (a sign-in's
 * codes, Hidden Apps, Remove Home+) opens Home+ on its page.
 *
 * A row's kind: step (OK, Left and Right change it), check (a choice: OK picks it), tint (colours: Left and
 * Right pick one), home (OK opens Home+ on `page`), app (OK opens `app`).
 */
'use strict';

const DIM = [1, 2, 5, 10, -1], OFF = [5, 10, 20, 30, 60, -1]; /* minutes; -1: never */
const DIM_DEFAULT = 2, OFF_DEFAULT = 10; /* home.h: IDLE_DIM_MS, IDLE_OFF_MS */
const DETAILS = [['off', 'Off'], ['compact', 'Compact'], ['full', 'Full']];
const LOOKS = [['default', 'Default', 'Each app’s own colours.'], ['dark', 'Dark', 'Each logo on a dark tile.'],
  ['clear', 'Clear', 'Frosted glass, with white logos.'], ['tinted', 'Tinted', 'Glass in a colour, with the logo in a pale tint of it.']];
const TINTS = [['Red', '#ff453a'], ['Orange', '#ff9f0a'], ['Yellow', '#ffd60a'], ['Green', '#30d158'], ['Teal', '#40c8e0'],
  ['Blue', '#0a84ff'], ['Indigo', '#5e5ce6'], ['Purple', '#bf5af2'], ['Pink', '#ff375f'], ['Graphite', '#8e8e93']];
const TINT_DEFAULT = 'Indigo';
const APPS_MS = 60000; /* how long a look at the TV's apps is good for */

// Other apps for Jellyfin and Plex, whose names don't say so (the launcher's CLIENTS: util.c)
const CLIENTS = { 'org.litefin.app': 'jellyfin', 'org.moonfin.webos': 'jellyfin', 'com.breezyfin.app': 'jellyfin', 'com.ocenfin.app': 'jellyfin',
  'com.beb.plxnative': 'plex' };

// the sign-in an app's Top Shelf uses, as the launcher works it out (util.c: media_source)
function source(app) {
  const name = `${app.id} ${app.title}`.toLowerCase();
  if (CLIENTS[app.id]) return CLIENTS[app.id];
  return name.includes('stremio') ? 'stremio' : name.includes('jellyfin') ? 'jellyfin' : name.includes('plex') || app.id === 'cdp-30' ? 'plex' : '';
}

// the choice after cur (or, dir -1, before it), going round; one not on the list counts as past its end
function along(list, cur, dir) {
  const i = list.indexOf(cur);
  return list[((i < 0 ? list.length : i) + (dir < 0 ? list.length - 1 : 1)) % list.length];
}

const minutes = (v, dflt) => (v < 0 ? 'Never' : `${v > 0 ? v : dflt} min`);

module.exports = function createSettings({ log, readConfig, setPrefs, home, setHome, apps, tvApps, accounts, version, elevated }) {
  const step = (id, label, value, desc, set) => ({ id, kind: 'step', label, value, desc, set });
  const inHome = (id, label, value, desc, page) => ({ id, kind: 'home', label, value, desc, page });
  // a pref that's on or off
  const onOff = (id, label, on, desc, key) => step(id, label, on ? 'On' : 'Off', desc, () => setPrefs({ [key]: !on }));

  function remote(h) {
    const on = h.mode !== 'off', rows = [
      step('homeButton', 'Home Button', on ? 'Opens Home+' : 'Opens LG Home', 'Home+ in place of LG’s home screen, after apps too.',
        () => setHome({ mode: on ? 'off' : h.noFlashAvailable ? 'redirect' : 'watch', asked: true }))
    ];
    if (on && h.noFlashAvailable) {
      rows.push(step('noFlash', 'No Flash', h.noFlashActive ? 'On' : 'Off', 'LG’s home screen never shows, not even for a moment.',
        () => setHome({ mode: h.mode === 'watch' ? 'redirect' : 'watch' })));
    }
    if (h.settingsKey.available) { /* Home+'s key filter or LG Input Hook catches the keys */
      rows.push(step('settingsButton', 'Settings Button', h.settingsKey.on ? 'Control Center' : 'LG Quick Settings', 'The Control Center, over other apps too.',
        () => setHome({ settingsKey: !h.settingsKey.on })));
      rows.push(step('inputButton', 'Input Button', h.inputKey.on ? 'Home+ Inputs' : 'LG Input Picker', 'The Control Center’s inputs, over other apps too.',
        () => setHome({ inputKey: !h.inputKey.on })));
    }
    const body = !h.noFlashAvailable && !h.settingsKey.available
      ? 'On this TV, LG Input Hook from Homebrew Channel opens Home+\nwithout LG’s home screen showing first (see the README).' : '';
    return { rows, body };
  }

  function display(p) {
    const dim = p.dimMinutes || DIM_DEFAULT, off = p.offMinutes || OFF_DEFAULT;
    const details = DETAILS.find(([id]) => id === p.topShelfDetails) || DETAILS[2];
    return { rows: [
      step('dimAfter', 'Dim After', minutes(p.dimMinutes, DIM_DEFAULT), 'With no key pressed, all but the focused tile dims.',
        (dir) => setPrefs({ dimMinutes: along(DIM, dim, dir) })),
      step('offAfter', 'Turn Off After', minutes(p.offMinutes, OFF_DEFAULT), 'Then the screen fades to black. A key wakes it.',
        (dir) => setPrefs({ offMinutes: along(OFF, off, dir) })),
      onOff('pixelShift', 'Pixel Shift', p.pixelShift !== false, 'The picture moves a pixel every minute.', 'pixelShift'),
      onOff('calmColours', 'Calm Colours', p.calmColours !== false, 'Dark app backgrounds, so ambient lights stay calm.', 'calmColours'),
      step('topShelf', 'Top Shelf', p.shelfTurns === false ? 'Stays Still' : 'Turns Pages', 'A new title every 10 seconds, or only when you ask.',
        () => setPrefs({ shelfTurns: p.shelfTurns === false })),
      step('topShelfDetails', 'Top Shelf Details', details[1], 'Off: open hint. Compact: genre and rating. Full: synopsis.',
        (dir) => setPrefs({ topShelfDetails: along(DETAILS.map(([id]) => id), details[0], dir) }))
    ] };
  }

  function tiles(p) {
    const look = LOOKS.find(([id]) => id === p.iconLook) || LOOKS[0];
    const on = TINTS.findIndex(([name]) => name.toLowerCase() === String(p.iconTint || '').toLowerCase());
    const tint = on < 0 ? TINTS.findIndex(([name]) => name === TINT_DEFAULT) : on;
    const rows = LOOKS.map(([id, label, desc]) => ({ id: `look-${id}`, kind: 'check', label, desc, on: id === look[0], set: () => setPrefs({ iconLook: id }) }));
    if (look[0] === 'tinted') {
      rows.push({ id: 'tint', kind: 'tint', label: TINTS[tint][0], desc: 'Left and Right pick another colour.',
        colours: TINTS.map((t) => t[1]), on: tint,
        set: (dir) => { /* Left and Right stop at the ends; OK goes round, as it does in Home+ */
          const to = TINTS[dir === 'next' ? (tint + 1) % TINTS.length : Math.max(0, Math.min(TINTS.length - 1, tint + dir))];
          return setPrefs({ iconTint: to[0] });
        } });
    }
    return { rows };
  }

  function general(p, hidden) {
    const rows = [
      step('sounds', 'Sounds', p.sounds === false ? 'Off' : 'On', 'A soft tick when focus moves.', () => setPrefs({ sounds: p.sounds === false })),
      step('clock', 'Clock', p.clock24 ? '24-Hour' : '12-Hour', 'The clock at the top right.', () => setPrefs({ clock24: !p.clock24 }))
    ];
    if (hidden) rows.push(inHome('hidden', 'Hidden Apps', String(hidden), 'Apps you’ve hidden, to show them again.', 'general'));
    return { rows };
  }

  function signIns(a, installed) {
    const rows = [], has = (src) => a[src].connected || installed.has(src);
    if (has('stremio')) {
      rows.push(inHome('stremio', 'Stremio', a.stremio.connected ? 'Signed In' : 'Not Signed In', 'What you’re partway through, with your Trakt progress.', 'accounts'));
    }
    rows.push(inHome('trakt', 'Trakt', a.trakt.connected ? 'Connected' : a.trakt.configured ? 'Not Connected' : 'Not Set Up', 'Paused playback, in Stremio’s Top Shelf.', 'accounts'));
    for (const [src, label] of [['jellyfin', 'Jellyfin'], ['plex', 'Plex']]) {
      if (has(src)) rows.push(inHome(src, label, a[src].connected ? 'Connected' : 'Not Connected', 'What you’re partway through on your server.', 'accounts'));
    }
    rows.push(inHome('tmdb', 'TMDB', a.tmdb.configured ? 'Set Up' : 'Not Set Up', 'What’s popular on your streaming apps.', 'accounts'));
    return { rows, body: 'Each app’s Continue Watching, or what’s popular on it, shows when you focus it in the top row.' };
  }

  const about = () => ({
    rows: [Object.assign(inHome('remove', 'Remove Home+', '', 'Undoes everything Home+ set up, then uninstalls it.', 'about'), { destructive: true })],
    body: `Home+ ${version}, ${elevated() ? 'set up with Homebrew Channel' : 'not set up yet'}\nMIT licence · github.com/pbalazaar/home-plus\n` +
      'Not affiliated with LG, Apple, Stremio, Trakt, Jellyfin or Plex.\nUses the TMDB API but is not endorsed or certified by TMDB.'
  });

  // The TV's apps, for Accounts and TV Settings: they don't change while a panel's open, and each key press
  // reads the pages twice, so webOS's long list isn't asked for each time
  let known = null, knownAt = 0;
  async function theApps() {
    if (!known || Date.now() - knownAt > APPS_MS) {
      const [all, tv] = await Promise.all([apps(), tvApps()]);
      known = { installed: new Set(all.map(source)), tv };
      knownAt = Date.now();
    }
    return known;
  }

  // Settings' pages, as the launcher lists them, those this TV has a use for
  async function pages() {
    const cfg = readConfig(), p = cfg.prefs || {};
    const [h, { installed, tv }] = await Promise.all([home(), theApps()]);
    const list = [
      Object.assign({ id: 'remote', name: 'Remote', about: 'What the Home, Settings and Input buttons open.', icon: 'remote' }, remote(h)),
      Object.assign({ id: 'display', name: 'Display & OLED', about: 'Looking after the screen while Home+ is on it.', icon: 'brightness' }, display(p)),
      Object.assign({ id: 'tiles', name: 'App Tiles', about: 'How every app’s tile looks.', icon: 'tiles' }, tiles(p))
    ];
    if (h.settingsKey.available && h.piccap) {
      list.push({ id: 'controlCenter', name: 'Control Center', about: 'The Control Center over other apps.', icon: 'controls',
        rows: [onOff('blurVideo', 'Blur Over Video', !!p.ccBlurVideo, 'Pauses PicCap while the Control Center is open.', 'ccBlurVideo')],
        body: 'Over other apps, the Control Center’s glass blurs what’s behind it.\nThe TV captures video for one app at a time, and PicCap has it:\n' +
          'Blur Over Video pauses PicCap while the Control Center is open,\nso its glass can blur the video too.' });
    }
    list.push(Object.assign({ id: 'general', name: 'General', about: 'Sounds, the clock and hidden apps.', icon: 'settings' },
      general(p, ((cfg.layout || {}).hidden || []).length)));
    list.push({ id: 'accessibility', name: 'Accessibility', about: 'Less motion, and less see-through glass.', icon: 'access', rows: [
      onOff('reduceMotion', 'Reduce Motion', !!p.reduceMotion, 'Menus appear in place, without growing or sliding.', 'reduceMotion'),
      onOff('reduceTransparency', 'Reduce Transparency', !!p.reduceTransparency, 'Glass is less see-through, so text stands out.', 'reduceTransparency')
    ] });
    list.push(Object.assign({ id: 'accounts', name: 'Accounts', about: 'Continue Watching, and what’s popular on your apps.', icon: 'person' },
      signIns(accounts(), installed)));
    if (tv.length) {
      list.push({ id: 'tv', name: 'TV Settings', about: 'LG’s own settings.', icon: 'tv',
        rows: tv.map((t) => ({ id: `tv-${t.id}`, kind: 'app', label: t.label, value: '', desc: '', app: t.id })) });
    }
    list.push(Object.assign({ id: 'about', name: 'About', about: 'Home+ itself, and removing it.', icon: 'info' }, about()));
    return list;
  }

  async function state() {
    return { returnValue: true, pages: await pages(), prefs: readConfig().prefs || {} };
  }

  // changes a row, from how things stand now (the panel asks for one at a time, for that)
  async function act(id, dir) {
    const row = [].concat(...(await pages()).map((p) => p.rows)).find((r) => r.id === id);
    if (!row || !row.set) throw new Error(`not a setting: ${id}`);
    await row.set(dir);
    log('settings:', id);
  }

  return { state, act };
};
