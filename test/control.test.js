'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const createControl = require('../service/control');

/* A stand-in TV: answers the luna calls the Control Center makes, and records them. */
function fakeTv({ arc = true, livetv = true, wifi = 'Home Wi-Fi', wired = false, output = 'external_arc', sleep = 'off' } = {}) {
  const tv = { soundOutput: output, sleepTimer: sleep }, calls = [];
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
    if (uri.endsWith('/getSystemSettings')) return { returnValue: true, settings: { [params.keys[0]]: tv[params.keys[0]] } };
    if (uri.endsWith('/setSystemSettings')) { Object.assign(tv, params.settings); return { returnValue: true }; }
    return { returnValue: true };
  };
  return { control: createControl({ luna, log: () => {} }), tv, calls };
}

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

test('control: no Wi-Fi: Ethernet, or not connected', async () => {
  assert.deepEqual((await fakeTv({ wifi: '', wired: true }).control.state()).network, { kind: 'wired', name: 'Ethernet' });
  assert.deepEqual((await fakeTv({ wifi: '' }).control.state()).network, { kind: 'none', name: 'Not Connected' });
});

test('control: without an ARC device the only output is the TV\'s speakers (plus whatever is on now)', async () => {
  assert.deepEqual((await fakeTv({ arc: false, output: 'tv_speaker' }).control.state()).sound.outputs, ['tv_speaker']);
  assert.deepEqual((await fakeTv({ arc: false, output: 'bt_soundbar' }).control.state()).sound.outputs, ['tv_speaker', 'bt_soundbar']);
});

test('control: Sound steps through the outputs', async () => {
  const { control, tv } = fakeTv({ output: 'tv_speaker' });
  assert.deepEqual(await control.act('sound', 'next'), { soundOutput: 'external_arc' });
  assert.equal(tv.soundOutput, 'external_arc');
  await control.act('sound', 'next');
  assert.equal(tv.soundOutput, 'tv_speaker');
  await assert.rejects(control.act('sound', 'loudspeaker'), /not a sound output/);
});

test('control: Sleep Timer steps through LG\'s own values', async () => {
  const { control, tv } = fakeTv();
  const seen = [];
  for (let i = 0; i < 9; i++) seen.push((await control.act('sleepTimer', 'next')).sleepTimer);
  assert.deepEqual(seen, ['10', '20', '30', '60', '90', '120', '180', '240', 'off']);
  assert.equal(tv.sleepTimer, 'off');
  await assert.rejects(control.act('sleepTimer', '45'), /sleep timer/);
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
