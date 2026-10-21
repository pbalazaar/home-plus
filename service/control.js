/* Control Center: the TV's quick controls, like tvOS's: power, screen off, sound
 * output, sleep timer, picture and inputs, through webOS's own services (this service
 * runs as root). Which controls it shows, in what order and size, is yours to change
 * (like iOS 18's): the catalogue is below, and the list is `controlCenter` in
 * config.json. Every action is also the luna method `control`, so LG Input Hook can
 * put one on a remote button.
 */
'use strict';

const POWER = 'luna://com.webos.service.tvpower/power';
const SETTINGS = 'luna://com.webos.settingsservice';
// The sleep timer that turns the TV off (what LG's Quick Settings use; the time/sleepTimer
// setting doesn't start it), in LG's steps
const TIMER = 'luna://com.webos.service.tvReservationAgent/timer/sleep';
const SLEEP = ['off', '10', '20', '30', '60', '90', '120', '180', '240'];
// Sound switches between the TV's speakers and HDMI ARC (always: with the output on the speakers,
// webOS stops reporting an ARC device that's asleep, and switching to ARC wakes it over CEC)
const TOGGLE = ['tv_speaker', 'external_arc'];
const OUTPUT_NAMES = {
  tv_speaker: 'TV Speakers', tv_speakerbar: 'TV Speakers', external_arc: 'HDMI ARC', external_optical: 'Optical',
  bt_soundbar: 'Bluetooth', wisa_speaker: 'WiSA Speakers', mobile_phone: 'Phone', lineout: 'Line Out',
  builtin_soundbar: 'Soundbar', headphone: 'Headphones'
};

// LG's names for its picture modes (its Quick Settings' strings); SDR, HDR and Dolby Vision
// content each have their own ids
const PICTURE_NAMES = {
  normal: 'Standard', hdrStandard: 'Standard', dolbyHdrStandard: 'Standard',
  vivid: 'Vivid', hdrVivid: 'Vivid', dolbyHdrVivid: 'Vivid',
  eco: 'Eco', hdrEco: 'Eco',
  cinema: 'Cinema', hdrCinema: 'Cinema', hdrCinemaBright: 'Cinema Home', dolbyHdrCinemaBright: 'Cinema Home',
  sports: 'Sports', photo: 'Photo',
  game: 'Game Optimizer', hdrGame: 'Game Optimizer', dolbyHdrGame: 'Game Optimizer',
  filmMaker: 'Filmmaker Mode', hdrFilmMaker: 'Filmmaker Mode', dolbyHdrCinema: 'Filmmaker Mode',
  expert1: 'Expert (Bright)', expert2: 'Expert (Dark)',
  personalized: 'Personalized', hdrPersonalized: 'Personalized', dolbyHdrPersonalized: 'Personalized'
};

// the other TV settings a control can step through, by LG's names (its Quick Settings' strings)
const SOUND_MODES = { aiSoundPlus: 'AI Sound Pro', aiSound: 'AI Sound', standard: 'Standard', movie: 'Cinema', news: 'Clear Voice',
  sports: 'Sports', music: 'Music', game: 'Game' };
const ENERGY = { auto: 'Auto', off: 'Off', min: 'Minimum', med: 'Medium', max: 'Maximum' };
const ON_OFF = { on: 'On', off: 'Off' };
const BRIGHTNESS = [20, 40, 60, 80, 100]; /* OLED Pixel Brightness, 0-100: the levels it steps through */

// Sizes: Small is one round unit, Wide two side by side, Large two by two. The launcher places
// them in order on a grid four units wide; the service only caps the list.
const UNITS = { small: 1, wide: 2, large: 4 };
const MAX_UNITS = 28;
const DEFAULT_LAYOUT = [['power', 'large'], ['network', 'wide'], ['screenOff', 'wide'], ['sound', 'wide'], ['sleepTimer', 'wide'],
  ['pictureMode', 'wide'], ['eyeComfort', 'wide'], ['inputs', 'small'], ['recents', 'small'], ['accessibility', 'small'],
  ['settings', 'small']].map(([id, size]) => ({ id, size }));

/* extra: more catalogue entries ({ id, title, kind, icon, group, sizes, available(), read(), run() }),
 * like ambient.js's */
module.exports = function createControl({ luna, log, readConfig = () => ({}), writeConfig = () => {}, extra = [] }) {
  // the other outputs this TV has used (Optical, Bluetooth...), so Sound can switch back to them
  const used = new Set(readConfig().soundOutputs || []);
  function remember(out) {
    if (!out || TOGGLE.includes(out) || !OUTPUT_NAMES[out] || used.has(out)) return;
    used.add(out);
    const cfg = readConfig();
    cfg.soundOutputs = [...used];
    writeConfig(cfg);
  }

  async function settings(category, keys) {
    const r = await luna(`${SETTINGS}/getSystemSettings`, { category, keys });
    return (r && r.settings) || {}; /* a key this TV doesn't have fails the call but not the others */
  }

  // how webOS describes a setting: the values it offers now (visible and active, as LG's Quick
  // Settings list them), or its range; whether it can be changed now (HDR settings can't while
  // SDR plays); null when this TV doesn't have it
  async function describe(category, key) {
    const r = await luna(`${SETTINGS}/getSystemSettingDesc`, { category, keys: [key] });
    const d = r && r.returnValue !== false && r.results && r.results[0];
    if (!d) return null;
    const v = d.values || {};
    return { values: (v.arrayExt || []).filter((m) => m.visible && m.active).map((m) => m.value), range: v.range || null,
      usable: !(d.ui && d.ui.active === false) };
  }

  // minutes until the sleep timer turns the TV off (0: it's off), and its step (LG shows the
  // step it's within: 47 minutes left is the 60-minute timer)
  async function sleepLeft() {
    const r = await luna(`${TIMER}/getRemainTime`, {});
    return r && r.returnValue !== false ? (Number(r.sleepTimeMinute) || 0) + (Number(r.sleepTimeSecond) || 0) / 60 : 0;
  }
  // the picture modes LG offers for what's on now (as its Quick Settings list them)
  async function pictureModes() {
    const d = await describe('picture', 'pictureMode');
    return ((d && d.values) || []).map((id) => ({ id, label: PICTURE_NAMES[id] || id }));
  }
  const sleepStep = (left) => (left > 0 ? SLEEP.find((m) => m !== 'off' && left <= Number(m)) || '240' : 'off');

  // the inputs: Live TV, then LG's names for the HDMI ports (a CEC device's, like
  // "HW-Q990F") and what's behind them ("PS5")
  async function inputs() {
    const [r, apps] = await Promise.all([luna('luna://com.webos.service.eim/getAllInputStatus', {}),
      luna('luna://com.webos.applicationManager/listApps', {})]);
    const live = ((apps && apps.apps) || []).some((a) => a.id === 'com.webos.app.livetv');
    return {
      list: (live ? [{ appId: 'com.webos.app.livetv', label: 'Live TV', device: '', plugged: true }] : []).concat(
        ((r && r.devices) || []).filter((d) => d.appId).map((d) => ({
          appId: d.appId, label: d.label || d.id, device: d.spdProductDescription || '', plugged: !!d.hdmiPlugIn
        })))
    };
  }

  const next = (list, cur) => list[(list.indexOf(cur) + 1) % list.length];

  // webOS answers a refusal with returnValue false: that's an error, so the launcher says so
  function ok(r) {
    if (!r || r.returnValue === false) throw new Error((r && r.errorText) || "the TV didn't answer");
    return r;
  }

  // A control over one TV setting: OK steps through the values the TV offers (and wraps), or turns a
  // switch on and off. names: LG's names for the values; steps: for a range, the levels it steps
  // through; off: the value that means it's off, for one that's on or off (tvOS shows which).
  function setting(category, key, { names = {}, steps = null, kind = 'stepper', skip = [], off } = {}) {
    return {
      kind,
      available: async () => !!(await describe(category, key)),
      read: async () => {
        const [d, s] = await Promise.all([describe(category, key), settings(category, [key])]);
        const v = s[key];
        if (!d || v === undefined) return { available: false };
        return { available: d.usable, value: names[v] || String(v), on: off === undefined ? null : v !== off };
      },
      run: async () => {
        const d = await describe(category, key);
        if (!d || !d.usable) throw new Error(`${key} can't be changed now`);
        const cur = (await settings(category, [key]))[key];
        let v;
        if (steps) v = steps.find((n) => n > Number(cur)) || steps[0]; /* a value in between goes up to the next level */
        else {
          const values = d.values.filter((x) => !skip.includes(x));
          if (!values.length) throw new Error(`the TV offers no ${key} now`);
          v = next(values, cur);
        }
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category, settings: { [key]: v } }));
        log('control:', key, v);
      }
    };
  }

  // The catalogue: everything that can be placed. kind: action (does something), opens
  // (somewhere), list (a list to choose from), stepper (OK goes to the next value), switch.
  // The built-in ones are read with the panel's state; the rest read themselves.
  const always = async () => true;
  const CATALOG = [
    { id: 'power', title: 'Power Off', kind: 'action', icon: 'power', group: 'TV', sizes: ['large', 'small'], available: always },
    { id: 'screenOff', title: 'Screen Off', kind: 'action', icon: 'screenOff', group: 'TV', sizes: ['wide', 'small'], available: always },
    { id: 'network', title: 'Wi-Fi', kind: 'opens', icon: 'wifi', group: 'TV', sizes: ['wide'], available: always },
    { id: 'sleepTimer', title: 'Sleep Timer', kind: 'stepper', icon: 'timer', group: 'TV', sizes: ['wide', 'small'], available: always,
      run: () => act('sleepTimer', 'next') },
    { id: 'inputs', title: 'Inputs', kind: 'list', icon: 'input', group: 'TV', sizes: ['small'], available: always },
    { id: 'pictureMode', title: 'Picture', kind: 'list', icon: 'picture', group: 'Picture & Sound', sizes: ['wide', 'small'], available: always },
    { id: 'eyeComfort', title: 'Eye Comfort', kind: 'switch', icon: 'eye', group: 'Picture & Sound', sizes: ['wide', 'small'], available: always,
      run: () => act('eyeComfort', 'next') },
    { id: 'sound', title: 'Sound', kind: 'stepper', icon: 'sound', group: 'Picture & Sound', sizes: ['wide', 'small'], available: always,
      run: () => act('sound', 'next') },
    Object.assign({ id: 'soundMode', title: 'Sound Mode', icon: 'soundMode', group: 'Picture & Sound', sizes: ['wide', 'small'] },
      setting('sound', 'soundMode', { names: SOUND_MODES })),
    Object.assign({ id: 'brightness', title: 'OLED Brightness', icon: 'brightness', group: 'Picture & Sound', sizes: ['wide', 'small'] },
      setting('picture', 'backlight', { steps: BRIGHTNESS })),
    Object.assign({ id: 'energySaving', title: 'Energy Saving', icon: 'energy', group: 'Picture & Sound', sizes: ['wide', 'small'] },
      setting('picture', 'energySaving', { names: ENERGY, skip: ['screen_off', 'screenOff'], off: 'off' })), /* Screen Off does that */
    Object.assign({ id: 'gameOptimizer', title: 'Game Optimizer', icon: 'game', group: 'Picture & Sound', sizes: ['wide', 'small'] },
      setting('other', 'gameOptimization', { names: ON_OFF, kind: 'switch', off: 'off' })),
    Object.assign({ id: 'toneMapping', title: 'Dynamic Tone Mapping', icon: 'hdr', group: 'Picture & Sound', sizes: ['wide', 'small'] },
      setting('picture', 'hdrDynamicToneMapping', { names: { on: 'On', off: 'Off', HGIG: 'HGIG' }, off: 'off' })),
    ...extra,
    { id: 'recents', title: 'Recents', kind: 'opens', icon: 'recents', group: 'Home+', sizes: ['small'], available: always },
    { id: 'accessibility', title: 'Accessibility', kind: 'opens', icon: 'access', group: 'Home+', sizes: ['small'], available: always },
    { id: 'settings', title: 'Settings', kind: 'opens', icon: 'settings', group: 'Home+', sizes: ['small'], available: always, pinned: true }
  ];
  const byId = new Map(CATALOG.map((e) => [e.id, e]));

  // A list as saved, made one the launcher can show: unknown and repeated controls dropped, a size
  // a control doesn't have made its first, Settings (the only way into Settings) put back, capped.
  function clean(list) {
    const seen = new Set(), out = [];
    let units = 0;
    for (const c of Array.isArray(list) ? list : []) {
      const e = c && byId.get(c.id);
      if (!e || seen.has(e.id)) continue;
      const size = e.sizes.includes(c.size) ? c.size : e.sizes[0];
      if (units + UNITS[size] + (e.id === 'settings' || seen.has('settings') ? 0 : 1) > MAX_UNITS) continue; /* room kept for Settings */
      seen.add(e.id);
      out.push({ id: e.id, size });
      units += UNITS[size];
    }
    if (!out.length) return DEFAULT_LAYOUT.map((c) => Object.assign({}, c));
    if (!seen.has('settings')) out.push({ id: 'settings', size: 'small' });
    return out;
  }
  const layout = () => clean(readConfig().controlCenter || DEFAULT_LAYOUT);

  async function state() {
    const placed = layout();
    const [net, sound, left, inp, pic, modes, avail] = await Promise.all([
      luna('luna://com.webos.service.connectionmanager/getStatus', {}),
      settings('sound', ['soundOutput', 'eArcSupport']), sleepLeft(), inputs(),
      settings('picture', ['pictureMode', 'eyeComfortMode']), pictureModes(),
      Promise.all(CATALOG.map((e) => e.available().catch(() => false)))
    ]);
    const output = sound.soundOutput;
    const wifi = net && net.wifi && net.wifi.state === 'connected' ? net.wifi.ssid || net.wifi.displayName || 'Wi-Fi' : '';
    const wired = !!(net && net.wired && net.wired.state === 'connected');
    remember(output);
    const outputs = [...new Set([...TOGGLE, ...used])];
    const st = {
      network: wifi ? { kind: 'wifi', name: wifi } : { kind: wired ? 'wired' : 'none', name: wired ? 'Connected' : 'Not Connected' },
      sound: { output: output || '', label: output === 'external_arc' && sound.eArcSupport === 'on' ? 'HDMI eARC' : OUTPUT_NAMES[output] || output || '', outputs },
      sleepTimer: left > 0 ? String(Math.ceil(left)) : 'off', /* minutes left */
      picture: { mode: pic.pictureMode || '', label: PICTURE_NAMES[pic.pictureMode] || pic.pictureMode || '', modes },
      eyeComfort: pic.eyeComfortMode || '',
      inputs: inp.list
    };
    // the built-in controls' lines, from the state above. on: whether what it shows is on
    // (connected, a timer set, a sound system playing), for those that can be off
    const builtin = {
      network: { title: st.network.kind === 'wired' ? 'Ethernet' : st.network.kind === 'wifi' ? 'Wi-Fi' : 'Network',
        icon: st.network.kind === 'wired' ? 'ethernet' : 'wifi', value: st.network.name, on: st.network.kind !== 'none' },
      sleepTimer: { value: st.sleepTimer === 'off' ? 'Off' : `${st.sleepTimer} min`, on: st.sleepTimer !== 'off' },
      sound: { value: st.sound.label, on: !!output && !output.startsWith('tv_speaker') },
      pictureMode: { value: st.picture.label },
      eyeComfort: { value: st.eyeComfort === 'on' ? 'On' : 'Off', on: st.eyeComfort === 'on' }
    };
    // what each placed control shows (only those are read: one call each, while the panel is open)
    st.controls = await Promise.all(placed.map(async (c) => {
      const e = byId.get(c.id);
      let s = builtin[c.id] || {};
      if (e.read) s = await e.read().catch((err) => { log('control:', c.id, err.message); return { available: false }; });
      const available = avail[CATALOG.indexOf(e)] && s.available !== false;
      return Object.assign({ id: c.id, size: c.size, title: e.title, kind: e.kind, icon: e.icon, value: '', on: null },
        s, { available, value: available ? s.value || '' : 'Not Available' });
    }));
    // the gallery: everything that works on this TV, and whether it's placed
    st.catalog = CATALOG.map((e, i) => ({ id: e.id, title: e.title, kind: e.kind, icon: e.icon, group: e.group, sizes: e.sizes,
      available: !!avail[i], placed: placed.some((c) => c.id === e.id), pinned: !!e.pinned }));
    return st;
  }

  // action: powerOff, screenOff, sound (an output, or "next"), sleepTimer (minutes, "off" or
  // "next"), pictureMode (one of the modes on offer now), eyeComfort ("on", "off" or "next"),
  // input (an input's app id); for the panel's own list: run (a control's id: it steps or
  // switches), layout (body.controls: the list to save), reset (back to the default list)
  async function act(action, value, body = {}) {
    switch (action) {
      case 'powerOff':
        return ok(await luna(`${POWER}/powerOff`, { reason: 'remoteKey' }));
      case 'screenOff': /* "active": any key brings the picture back ("passive" can't) */
        return ok(await luna(`${POWER}/turnOffScreen`, { standbyMode: 'active' }));
      case 'sound': {
        const cur = (await settings('sound', ['soundOutput'])).soundOutput;
        remember(cur);
        const out = value === 'next' ? next([...new Set([...TOGGLE, ...used])], cur) : String(value || '');
        if (!OUTPUT_NAMES[out]) throw new Error(`not a sound output: ${out}`);
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'sound', settings: { soundOutput: out } }));
        return { soundOutput: out };
      }
      case 'sleepTimer': {
        const v = value === 'next' ? next(SLEEP, sleepStep(await sleepLeft())) : String(value || '');
        if (!SLEEP.includes(v)) throw new Error(`the sleep timer is off or ${SLEEP.slice(1).join(', ')} minutes`);
        ok(await luna(`${TIMER}/add`, { sleepTimeMinute: v === 'off' ? 0 : Number(v) }));
        return { sleepTimer: v };
      }
      case 'pictureMode': { /* for what's on now: LG keeps one per input and SDR/HDR/Dolby Vision */
        if (!(await pictureModes()).some((m) => m.id === value)) throw new Error(`not a picture mode the TV offers now: ${value}`);
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'picture', settings: { pictureMode: value } }));
        return { pictureMode: value };
      }
      case 'eyeComfort': {
        const cur = (await settings('picture', ['eyeComfortMode'])).eyeComfortMode;
        const v = value === 'next' ? (cur === 'on' ? 'off' : 'on') : String(value || '');
        if (v !== 'on' && v !== 'off') throw new Error('Eye Comfort is on or off');
        ok(await luna(`${SETTINGS}/setSystemSettings`, { category: 'picture', settings: { eyeComfortMode: v } }));
        return { eyeComfort: v };
      }
      case 'input': {
        const inp = await inputs();
        if (!inp.list.some((d) => d.appId === value)) throw new Error(`not an input: ${value}`);
        return ok(await luna('luna://com.webos.applicationManager/launch', { id: value }));
      }
      case 'run': {
        const e = byId.get(String(value || ''));
        if (!e || !e.run) throw new Error(`not a control that steps or switches: ${value}`);
        if (!(await e.available().catch(() => false))) throw new Error(`${e.title} isn't available on this TV`);
        await e.run();
        return { run: e.id };
      }
      case 'layout': {
        const list = clean(body.controls);
        const cfg = readConfig();
        cfg.controlCenter = list;
        writeConfig(cfg);
        return { controls: list };
      }
      case 'reset': {
        const cfg = readConfig();
        delete cfg.controlCenter;
        writeConfig(cfg);
        return { controls: layout() };
      }
      default:
        throw new Error(`unknown action: ${action}`);
    }
  }

  return { state, act, layout, clean };
};
