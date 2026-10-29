/* The remote's Home, Settings and Input keys without LG Input Hook. LG's compositor (surface-manager)
 * runs each key through a list of small JS filters, configd's com.webos.surfacemanager.keyFilters (a
 * file and a function in it, each); Home+ puts one of its own in that list, before LG's
 * handleSystemKeys (Home; Settings and Input are handleHotkeys', after it). It's a volatile value:
 * configd keeps it in memory only and hands it to the compositor at once, and a reboot clears it. A
 * filter that's missing or fails is skipped (webOS OSE's WebOSKeyFilter), so LG's own keep working.
 * Found on the C3 (webOS 23); a TV without the list has LG Input Hook's binds.
 *
 * The filter takes only the keys Home+ is asked to (Home: No Flash; Settings, Input: their buttons in
 * Settings) and does what LG Input Hook's binds do: Home opens Home+, Settings and Input ask the
 * service (POST /api/key), and each does what the key does without Home+ when Home+ is gone or doesn't
 * answer. The keys and the API's token are written into it, so it's written again, under a new name,
 * when they change (the compositor may keep a file it has loaded by its name).
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const CONFIGD = 'luna://com.webos.service.config';
const KEY_FILTERS = 'com.webos.surfacemanager.keyFilters';
const HANDLER = 'handleHomePlusKeys';
const BEFORE = 'handleSystemKeys'; /* LG's filter for Home */
const PREFIX = 'keyfilter-'; /* its files: keyfilter-<hash of what's in it>.js, beside config.json */
const KEYS = ['home', 'settings', 'input'];

const ours = (f) => !!f && f.handler === HANDLER;

// The compositor's list with Home+'s filter (file) in it, before LG's Home key's, or without it (no
// file). null: a list Home+ doesn't know (no handleSystemKeys), which it leaves as it is.
function withFilter(list, file) {
  if (!Array.isArray(list)) return null;
  const lg = list.filter((f) => !ours(f));
  if (!file) return lg;
  const at = lg.findIndex((f) => !!f && f.handler === BEFORE);
  if (at < 0) return null;
  return lg.slice(0, at).concat([{ file, handler: HANDLER }], lg.slice(at));
}

// The filter, as the compositor includes it into its key filter shell (Qt.include): LG's helpers
// (lunaCall, genericTimer, the views) are there; Home+'s own names start with homeplus.
function filterSource({ keys, api, token, appId, lgHome }) {
  const s = JSON.stringify;
  return `// Home+'s key filter, run by LG's compositor from configd's ${KEY_FILTERS}. Written by
// Home+'s service (service/keyfilter.js); a reboot, or Remove Home+, takes it out of the list.
// Home: Home+, else LG's home screen. Settings, Input: whatever Home+'s service makes of them (the
// Control Center, its inputs), else LG's Quick Settings and Input picker. Keys LG would ignore just
// now (in its first-time setup, under an alert) go on to LG's own filters.

var homeplusKeys = ${s(keys)};

function init() {
}

// what LG's own filters check before they act on these keys (launchHomeApp, handleHomeKeys, handleHotkeys)
function homeplusAllowed(key) {
    try {
        if (bootManager.bootFirstUse && !systemPropertyService.powerOnlyMode)
            return false;
        if (key === Qt.Key_Super_L)
            return !modalAlert.visible && !settingsService.disableHome;
    } catch (e) {
    }
    return true;
}

function homeplusAnswer(payload) {
    try {
        return typeof payload === "string" ? JSON.parse(payload) : (payload || {});
    } catch (e) {
        return {};
    }
}

// Home+, or LG's home screen when it doesn't open (Home+ is gone)
function homeplusHome() {
    lunaCall.send("com.webos.applicationManager", "/launch", ${s(s({ id: appId }))}, function (payload) {
        if (!homeplusAnswer(payload).returnValue)
            lunaCall.send("com.webos.applicationManager", "/launch", ${s(s({ id: lgHome }))});
    });
}

// Home+'s service, or LG's own system UI (lgApp) when it doesn't answer in 2 s
function homeplusAsk(key, lgApp) {
    var done = false, timer = 0, xhr = null;
    var lg = function () {
        if (done)
            return;
        done = true;
        lunaCall.send("com.webos.service.systemUIManager", "/launch", JSON.stringify({"appId": lgApp}));
    };
    try {
        xhr = new XMLHttpRequest();
        xhr.onreadystatechange = function () {
            if (xhr.readyState !== 4 || done)
                return;
            if (timer)
                genericTimer.cancel(timer);
            if (xhr.status === 200)
                done = true;
            else
                lg();
        };
        xhr.open("POST", ${s(api)});
        xhr.setRequestHeader("Content-Type", "application/json");
        xhr.setRequestHeader("X-Homeplus-Token", ${s(token)});
        xhr.send(JSON.stringify({"key": key}));
    } catch (e) {
        lg();
        return;
    }
    try {
        timer = genericTimer.set(2000, function () {
            lg();
            try { xhr.abort(); } catch (e) {}
        });
    } catch (e) {
    }
}

function ${HANDLER}(key, pressed, autoRepeat) {
    var name = key === Qt.Key_Super_L ? "home" : key === WebOS.Key_webOS_Settings ? "settings"
             : key === WebOS.Key_webOS_InputSource ? "input" : "";
    if (!name || !homeplusKeys[name] || !homeplusAllowed(key))
        return KeyPolicy.NextPolicy;
    if (pressed && !autoRepeat) {
        if (name === "home")
            homeplusHome();
        else if (name === "settings")
            homeplusAsk("settings", "com.webos.app.quicksettings");
        else
            homeplusAsk("input", "com.webos.app.quickinputpicker");
    }
    return KeyPolicy.Accepted;
}
`;
}

// luna(uri, params): a call's answer. subscribe(uri, params): webos-service's subscription. dir(): the
// folder its files go in (config.json's). write(file, text): written whole. token(): the API's.
module.exports = function createKeyFilter({ luna, subscribe, log, dir, write, api, token, appId, lgHome }) {
  let list = null; /* the compositor's list, as configd last said (null: it hasn't, or has none) */
  let wanted = null; /* the keys Home+ was asked to take: { home, settings, input } */
  let placed = null; /* the filter Home+ put in the list: { file, keys } */
  let failed = ''; /* why configd refused the last change */
  let busy = Promise.resolve();
  const available = () => !!list && withFilter(list, 'x') !== null;
  const pick = (keys) => KEYS.reduce((o, k) => Object.assign(o, { [k]: !!(keys && keys[k]) }), {});

  // its files but the one in the list (keep): a file the compositor no longer runs
  function tidy(keep) {
    try {
      for (const f of fs.readdirSync(dir())) {
        if (f.startsWith(PREFIX) && f.endsWith('.js') && path.join(dir(), f) !== keep) fs.unlinkSync(path.join(dir(), f));
      }
    } catch (e) { /* no folder yet */ }
  }

  async function settle() {
    failed = '';
    if (!wanted || !available()) return;
    const keys = pick(wanted);
    let file = null;
    if (KEYS.some((k) => keys[k])) {
      const text = filterSource({ keys, api, token: token(), appId, lgHome });
      file = path.join(dir(), PREFIX + crypto.createHash('sha1').update(text).digest('hex').slice(0, 12) + '.js');
      if (!fs.existsSync(file)) write(file, text); /* (its name is its contents) */
    }
    const there = list.find(ours);
    if (there ? there.file === file : !file) { /* the list has it as wanted already */
      placed = file ? { file, keys } : null;
      return tidy(file);
    }
    const r = await luna(CONFIGD + '/setConfigs', { configs: { [KEY_FILTERS]: file ? withFilter(list, file) : null }, volatile: true });
    if (!r || r.returnValue === false) {
      failed = String((r && r.errorText) || 'no answer');
      log('key filter: configd refused it:', failed);
      return;
    }
    failed = '';
    list = withFilter(list, file); /* (configd says so too, to its subscribers) */
    placed = file ? { file, keys } : null;
    log('key filter:', file ? KEYS.filter((k) => keys[k]).join(', ') : 'off');
    tidy(file);
  }

  // one change at a time, each with what's wanted by then
  function queue() {
    busy = busy.then(settle).catch((e) => log('key filter:', e.message));
    return busy;
  }

  // the compositor's list now, and whenever it changes: a configd reconfigure (its layers read again)
  // drops volatile values, so Home+'s filter is put back
  function watch() {
    try {
      const sub = subscribe(CONFIGD + '/getConfigs', { configNames: [KEY_FILTERS], subscribe: true });
      sub.on('response', (m) => {
        const p = (m && m.payload) || {};
        if (!p.configs && !p.missingConfigs) return;
        const now = p.configs && p.configs[KEY_FILTERS];
        list = Array.isArray(now) ? now : null;
        if (placed && !(list && list.some((f) => ours(f) && f.file === placed.file))) placed = null;
        queue();
      });
      sub.on('cancel', () => setTimeout(watch, 3000));
    } catch (e) {
      log('key filter: configd watch failed', e.message);
      setTimeout(watch, 10000);
    }
  }

  return {
    start: watch,
    // the compositor has a list Home+ can put its filter in
    available,
    // the filter in the list takes this key
    takes: (key) => !!placed && !!placed.keys[key],
    // the keys to take now: { home, settings, input }. true, or why not (configd refused, or didn't answer)
    set(keys) {
      wanted = pick(keys);
      return queue().then(() => failed || true);
    },
    // Remove Home+: out of the list, and its files gone. true, or why not
    remove() {
      wanted = pick({});
      return queue().then(() => {
        tidy(null);
        return failed || true;
      });
    }
  };
};

module.exports.withFilter = withFilter;
module.exports.filterSource = filterSource;
module.exports.HANDLER = HANDLER;
module.exports.KEY_FILTERS = KEY_FILTERS;
