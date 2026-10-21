'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const createControl = require('../service/control');

/* A stand-in TV: answers the luna calls the Control Center makes, and records them. */
function fakeTv({ arc = true, livetv = true, wifi = 'Home Wi-Fi', wired = false, output = 'external_arc', earc = 'off', sleep = 0,
  readConfig, writeConfig, extra, has = Object.keys(DESC), sdr = true } = {}) {
  const tv = { soundOutput: output, eArcSupport: earc, sleepMinutes: sleep, pictureMode: 'expert1', eyeComfortMode: 'off',
    soundMode: 'standard', backlight: 47, energySaving: 'off', gameOptimization: 'off', hdrDynamicToneMapping: 'on' }, calls = [];
  const luna = async (uri, params) => {
    calls.push({ uri, params });
    if (uri.endsWith('/getAllInputStatus')) {
      return { returnValue: true, arcDevice: arc, devices: [
        { id: 'HDMI_1', appId: 'com.webos.app.hdmi1', label: 'HDMI 1', hdmiPlugIn: false },
        { id: 'HDMI_2', appId: 'com.webos.app.hdmi2', label: 'HW-Q990F', spdProductDescription: 'PS5', hdmiPlugIn: true },
        { id: 'BROKEN' }] };
    }
    if (uri.endsWith('/listApps')) return { returnValue: true, apps: livetv ? [{ id: 'com.webos.app.livetv' }] : [] };
    if (uri.endsWith('connectionmanager/getStatus')) {
      return { returnValue: true, wifi: wifi ? { state: 'connected', ssid: wifi } : { state: 'disconnected' }, wired: { state: wired ? 'connected' : 'disconnected' } };
    }
    if (uri.endsWith('/getSystemSettingDesc')) { /* as webOS 23 describes them (the C3, playing SDR) */
      const key = params.keys[0];
      if (!has.includes(key)) return { returnValue: false, errorText: 'no such key' };
      const d = DESC[key];
      const ui = { active: !(sdr && d.hdr) };
      return { returnValue: true, results: [{ key, ui, values: d.range ? { range: d.range } :
        { arrayExt: d.values.map((value) => ({ value, visible: true, active: true })).concat((d.hidden || []).map((value) => ({ value, visible: false, active: true }))) } }] };
    }
    if (uri.endsWith('/getSystemSettings')) return { returnValue: true, settings: Object.fromEntries(params.keys.map((k) => [k, tv[k]])) };
    /* LG's sleep timer: minutes and seconds left (no minutes when it's off) */
    if (uri.endsWith('/timer/sleep/getRemainTime')) return Object.assign({ returnValue: true, sleepTimeSecond: 0 }, tv.sleepMinutes ? { sleepTimeMinute: tv.sleepMinutes - 1, sleepTimeSecond: 59 } : {});
    if (uri.endsWith('/timer/sleep/add')) { tv.sleepMinutes = params.sleepTimeMinute; return { returnValue: true }; }
    if (uri.endsWith('/setSystemSettings')) { Object.assign(tv, params.settings); return { returnValue: true }; }
    return { returnValue: true };
  };
  return { control: createControl({ luna, log: () => {}, readConfig, writeConfig, extra }), tv, calls };
}

/* the settings the C3 describes (getSystemSettingDesc): their values, or range; hdr: inactive in SDR */
const DESC = {
  pictureMode: { values: ['vivid', 'normal', 'cinema', 'filmMaker', 'expert1'], hidden: ['hdrVivid'] },
  soundMode: { values: ['aiSoundPlus', 'standard', 'movie', 'news'], hidden: ['bassBlast'] },
  backlight: { range: { min: 0, max: 100, interval: 1 } },
  energySaving: { values: ['auto', 'off', 'min', 'med', 'max', 'screen_off'] },
  gameOptimization: { values: ['on', 'off'] },
  hdrDynamicToneMapping: { values: ['on', 'off', 'HGIG'], hdr: true }
};
const memoryConfig = (cfg = {}) => ({ readConfig: () => JSON.parse(JSON.stringify(cfg)), writeConfig: (c) => { for (const k of Object.keys(cfg)) delete cfg[k]; Object.assign(cfg, c); }, cfg });

test('control: what the panel shows: network, sound output, sleep timer and inputs', async () => {
  const { control } = fakeTv();
  const st = await control.state();
  assert.deepEqual(st.network, { kind: 'wifi', name: 'Home Wi-Fi' });
  assert.deepEqual(st.sound, { output: 'external_arc', label: 'HDMI ARC', outputs: ['tv_speaker', 'external_arc'] });
  assert.equal(st.sleepTimer, 'off');
  assert.deepEqual(st.inputs, [
    { appId: 'com.webos.app.livetv', label: 'Live TV', device: '', plugged: true },
    { appId: 'com.webos.app.hdmi1', label: 'HDMI 1', device: '', plugged: false },
    { appId: 'com.webos.app.hdmi2', label: 'HW-Q990F', device: 'PS5', plugged: true }]);
});

test('control: no Wi-Fi: Ethernet (the tile says so; its line says "Connected"), or not connected', async () => {
  assert.deepEqual((await fakeTv({ wifi: '', wired: true }).control.state()).network, { kind: 'wired', name: 'Connected' });
  assert.deepEqual((await fakeTv({ wifi: '' }).control.state()).network, { kind: 'none', name: 'Not Connected' });
});

test('control: Sound offers HDMI ARC even while webOS reports no ARC device (a soundbar asleep)', async () => {
  const { control, tv } = fakeTv({ arc: false, output: 'tv_speaker' });
  assert.deepEqual((await control.state()).sound.outputs, ['tv_speaker', 'external_arc']);
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'external_arc');
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'tv_speaker', 'and back');
});

test('control: Sound also offers the other outputs the TV has used, and remembers them', async () => {
  const cfg = {};
  const opts = { readConfig: () => JSON.parse(JSON.stringify(cfg)), writeConfig: (c) => Object.assign(cfg, c) };
  assert.deepEqual((await fakeTv({ arc: false, output: 'bt_soundbar', ...opts }).control.state()).sound.outputs, ['tv_speaker', 'external_arc', 'bt_soundbar']);
  assert.deepEqual(cfg.soundOutputs, ['bt_soundbar']);
  assert.deepEqual((await fakeTv({ arc: false, output: 'tv_speaker', ...opts }).control.state()).sound.outputs, ['tv_speaker', 'external_arc', 'bt_soundbar'], 'after a restart too');
});

test('control: Sound steps through the outputs', async () => {
  const { control, tv } = fakeTv({ output: 'tv_speaker' });
  assert.deepEqual(await control.act('sound', 'next'), { soundOutput: 'external_arc' });
  assert.equal(tv.soundOutput, 'external_arc');
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'tv_speaker');
  await assert.rejects(control.act('sound', 'loudspeaker'), /not a sound output/);
});

test('control: Sleep Timer steps through LG\'s own values, on the timer that turns the TV off', async () => {
  const { control, tv, calls } = fakeTv();
  const seen = [];
  for (let i = 0; i < 9; i++) seen.push((await control.act('sleepTimer', 'next')).sleepTimer);
  assert.deepEqual(seen, ['10', '20', '30', '60', '90', '120', '180', '240', 'off']);
  assert.equal(tv.sleepMinutes, 0, 'off');
  assert.ok(calls.some((c) => c.uri === 'luna://com.webos.service.tvReservationAgent/timer/sleep/add' && c.params.sleepTimeMinute === 240));
  await assert.rejects(control.act('sleepTimer', '45'), /sleep timer/);
});

test('control: the Sleep Timer shows the minutes left, and Next goes on from the step they\'re in', async () => {
  const { control, tv } = fakeTv({ sleep: 47 });
  assert.equal((await control.state()).sleepTimer, '47');
  assert.equal((await control.act('sleepTimer', 'next')).sleepTimer, '90', '47 minutes left is the 60-minute timer');
  assert.equal(tv.sleepMinutes, 90);
});

test('control: HDMI ARC is "HDMI eARC" when eARC is on', async () => {
  assert.equal((await fakeTv({ earc: 'on' }).control.state()).sound.label, 'HDMI eARC');
  assert.equal((await fakeTv().control.state()).sound.label, 'HDMI ARC');
});

test('control: an input opens only if the TV has it', async () => {
  const { control, calls } = fakeTv();
  await control.act('input', 'com.webos.app.hdmi2');
  assert.deepEqual(calls.at(-1), { uri: 'luna://com.webos.applicationManager/launch', params: { id: 'com.webos.app.hdmi2' } });
  await assert.rejects(control.act('input', 'com.evil.app'), /not an input/);
});

test('control: Power Off and Screen Off ask the TV\'s power service', async () => {
  const { control, calls } = fakeTv();
  await control.act('powerOff');
  assert.deepEqual(calls.at(-1), { uri: 'luna://com.webos.service.tvpower/power/powerOff', params: { reason: 'remoteKey' } });
  await control.act('screenOff');
  assert.deepEqual(calls.at(-1), { uri: 'luna://com.webos.service.tvpower/power/turnOffScreen', params: { standbyMode: 'active' } });
  await assert.rejects(control.act('reboot'), /unknown action/);
});

test('control: the picture mode, with the modes LG offers for what\'s on now, by LG\'s names', async () => {
  const { control, tv } = fakeTv();
  const st = await control.state();
  assert.deepEqual(st.picture, { mode: 'expert1', label: 'Expert (Bright)', modes: [
    { id: 'vivid', label: 'Vivid' }, { id: 'normal', label: 'Standard' }, { id: 'cinema', label: 'Cinema' },
    { id: 'filmMaker', label: 'Filmmaker Mode' }, { id: 'expert1', label: 'Expert (Bright)' }] });
  await control.act('pictureMode', 'cinema');
  assert.equal(tv.pictureMode, 'cinema');
  await assert.rejects(control.act('pictureMode', 'hdrVivid'), /picture mode/, 'an HDR mode, while SDR is on');
  assert.equal(tv.pictureMode, 'cinema');
});

test('control: Eye Comfort turns on and off', async () => {
  const { control, tv } = fakeTv();
  assert.equal((await control.state()).eyeComfort, 'off');
  assert.equal((await control.act('eyeComfort', 'next')).eyeComfort, 'on');
  assert.equal(tv.eyeComfortMode, 'on');
  assert.equal((await control.act('eyeComfort', 'next')).eyeComfort, 'off');
  await assert.rejects(control.act('eyeComfort', 'bright'), /on or off/);
});

test('control: the default list is today\'s Control Center, and the catalogue offers what this TV has', async () => {
  const st = await fakeTv({ has: ['pictureMode', 'soundMode', 'backlight'] }).control.state();
  assert.deepEqual(st.controls.map((c) => `${c.id}:${c.size}`), ['power:large', 'network:wide', 'screenOff:wide', 'sound:wide', 'sleepTimer:wide',
    'pictureMode:wide', 'eyeComfort:wide', 'inputs:small', 'recents:small', 'accessibility:small', 'settings:small']);
  const cat = Object.fromEntries(st.catalog.map((e) => [e.id, e]));
  assert.equal(cat.soundMode.available, true);
  assert.equal(cat.brightness.available, true);
  assert.equal(cat.energySaving.available, false, 'not described by this TV');
  assert.equal(cat.toneMapping.available, false);
  assert.equal(cat.power.placed, true);
  assert.equal(cat.soundMode.placed, false);
  assert.equal(cat.settings.pinned, true);
});

test('control: a saved list is cleaned up: unknown, repeated, wrong sizes, Settings missing, too long', async () => {
  const { control } = fakeTv();
  assert.deepEqual(control.clean([{ id: 'nothing' }, { id: 'sound', size: 'large' }, { id: 'sound', size: 'wide' }, { id: 'recents', size: 'wide' }]),
    [{ id: 'sound', size: 'wide' }, { id: 'recents', size: 'small' }, { id: 'settings', size: 'small' }]);
  assert.equal(control.clean([]).length, 11, 'nothing usable: the default');
  assert.equal(control.clean('not a list').length, 11);
  const many = ['power', 'network', 'screenOff', 'sound', 'sleepTimer', 'pictureMode', 'eyeComfort', 'soundMode', 'brightness', 'energySaving',
    'gameOptimizer', 'toneMapping', 'recents', 'accessibility', 'settings'].map((id) => ({ id, size: id === 'power' ? 'large' : 'wide' }));
  assert.ok(control.clean(many).reduce((n, c) => n + { small: 1, wide: 2, large: 4 }[c.size], 0) <= 28, 'capped at 28 units');
});

test('control: the list is saved, read back, and reset', async () => {
  const mem = memoryConfig();
  const { control } = fakeTv(mem);
  await control.act('layout', undefined, { controls: [{ id: 'soundMode', size: 'small' }, { id: 'power', size: 'small' }] });
  assert.deepEqual(mem.cfg.controlCenter, [{ id: 'soundMode', size: 'small' }, { id: 'power', size: 'small' }, { id: 'settings', size: 'small' }]);
  assert.deepEqual((await control.state()).controls.map((c) => c.id), ['soundMode', 'power', 'settings']);
  await control.act('reset');
  assert.equal(mem.cfg.controlCenter, undefined);
  assert.equal((await control.state()).controls.length, 11);
});

test('control: steppers go through what the TV offers and wrap; brightness steps up by levels', async () => {
  const mem = memoryConfig({ controlCenter: [{ id: 'soundMode', size: 'wide' }, { id: 'brightness', size: 'wide' }, { id: 'energySaving', size: 'wide' }] });
  const { control, tv, calls } = fakeTv(mem);
  const st = await control.state();
  assert.deepEqual(st.controls.slice(0, 3).map((c) => c.value), ['Standard', '47', 'Off']);
  await control.act('run', 'soundMode');
  assert.equal(tv.soundMode, 'movie');
  await control.act('run', 'soundMode');
  await control.act('run', 'soundMode');
  assert.equal(tv.soundMode, 'aiSoundPlus', 'it wraps (and leaves out what the TV hides)');
  assert.ok(calls.some((c) => c.uri.endsWith('/setSystemSettings') && c.params.category === 'sound' && c.params.settings.soundMode === 'movie'));
  await control.act('run', 'brightness');
  assert.equal(tv.backlight, 60, '47 goes up to the next level');
  tv.backlight = 100;
  await control.act('run', 'brightness');
  assert.equal(tv.backlight, 20, 'and the top level wraps to the first');
  tv.energySaving = 'max';
  await control.act('run', 'energySaving');
  assert.equal(tv.energySaving, 'auto', 'LG\'s screen-off level is left out: Screen Off does that');
});

test('control: switches turn on and off; a setting the TV can\'t change now shows Not Available', async () => {
  const mem = memoryConfig({ controlCenter: [{ id: 'gameOptimizer', size: 'wide' }, { id: 'toneMapping', size: 'wide' }] });
  const { control, tv } = fakeTv(mem);
  await control.act('run', 'gameOptimizer');
  assert.equal(tv.gameOptimization, 'on');
  const st = await control.state();
  assert.deepEqual([st.controls[0].on, st.controls[0].value], [true, 'On']);
  assert.deepEqual([st.controls[1].available, st.controls[1].value], [false, 'Not Available'], 'tone mapping, while SDR plays');
  await assert.rejects(control.act('run', 'toneMapping'), /can't be changed now/);
  await assert.rejects(control.act('run', 'power'), /steps or switches/);
});

test('control: what can be off says whether it is on, which tvOS shows in the glyph; the rest say nothing', async () => {
  const ids = ['network', 'sleepTimer', 'sound', 'eyeComfort', 'energySaving', 'gameOptimizer', 'soundMode', 'brightness', 'power'];
  const mem = memoryConfig({ controlCenter: ids.map((id) => ({ id, size: id === 'power' ? 'large' : 'wide' })) });
  const on = async (opts) => Object.fromEntries((await fakeTv(Object.assign(opts, mem)).control.state()).controls.map((c) => [c.id, c.on]));
  assert.deepEqual(await on({ sleep: 30 }), { network: true, sleepTimer: true, sound: true, eyeComfort: false, energySaving: false,
    gameOptimizer: false, soundMode: null, brightness: null, power: null, settings: null }, 'connected, a timer set, a sound system playing');
  const off = await on({ wifi: '', output: 'tv_speaker' });
  assert.deepEqual([off.network, off.sleepTimer, off.sound], [false, false, false], 'not connected, no timer, the TV speakers');
});

test('control: extra controls (like ambient light) join the catalogue', async () => {
  let on = false;
  const extra = [{ id: 'lamp', title: 'Lamp', kind: 'switch', icon: 'ambient', group: 'Ambient Light', sizes: ['wide'],
    available: async () => true, read: async () => ({ on, value: on ? 'On' : 'Off' }), run: async () => { on = !on; } }];
  const mem = memoryConfig({ controlCenter: [{ id: 'lamp', size: 'wide' }] });
  const { control } = fakeTv(Object.assign({ extra }, mem));
  assert.equal((await control.state()).catalog.find((e) => e.id === 'lamp').group, 'Ambient Light');
  await control.act('run', 'lamp');
  assert.deepEqual((await control.state()).controls[0].value, 'On');
});
