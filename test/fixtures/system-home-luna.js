#!/usr/bin/env node
'use strict';
const fs = require('fs');
const file = process.env.HOMEPLUS_MOCK_LUNA_STATE;
const m = JSON.parse(fs.readFileSync(file, 'utf8'));
const uri = process.argv[process.argv.length - 2], p = JSON.parse(process.argv[process.argv.length - 1]);
m.calls.push({ uri, p });
let reply;
if (uri.endsWith('/getProfiles')) reply = { returnValue: true, profiles: { defaultApps: {
  support: true, categories: { home: 'com.webos.app.home' }
} } };
else if (uri.endsWith('/getAppInfo')) reply = { returnValue: true, appInfo: { id: p.id, appCategory: ['home'] } };
else if (uri.endsWith('/getSystemSettings')) reply = { returnValue: true, settings: { defaultApps: m.defaults } };
else if (uri.endsWith('/setDefaultApp')) { m.defaults.home = p.appId; reply = { returnValue: true }; }
else if (uri.endsWith('/setSystemSettings')) { m.defaults = p.settings.defaultApps; reply = { returnValue: true }; }
else if (uri.endsWith('/launch')) { m.foreground = p.id; reply = { returnValue: true }; }
else if (uri.endsWith('/launchDefaultApp')) { m.foreground = m.defaults.home; reply = { returnValue: true }; }
else if (uri.endsWith('/getForegroundAppInfo')) reply = { returnValue: true, appId: m.foreground };
else reply = { returnValue: false };
fs.writeFileSync(file + '.tmp', JSON.stringify(m)); fs.renameSync(file + '.tmp', file);
if (uri.endsWith('/getSystemSettings') && m.pauseRead === m.calls.filter((c) => c.uri.endsWith('/getSystemSettings')).length) {
  fs.writeFileSync(file + '.paused', '');
  const timer = setInterval(() => {
    if (!fs.existsSync(file + '.resume')) return;
    clearInterval(timer); console.log(JSON.stringify(reply));
  }, 10);
} else if (uri.endsWith('/getAppInfo') && m.healthDelay) setTimeout(() => console.log(JSON.stringify(reply)), m.healthDelay);
else console.log(JSON.stringify(reply));
