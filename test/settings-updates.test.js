'use strict';
const test = require('node:test');
const assert = require('assert');
const createSettings = require('../service/settings');
function settings(updates) {
 return createSettings({ updates, log:()=>{}, readConfig:()=>({}), setPrefs:()=>{}, home:async()=>({mode:'off',settingsKey:{available:false},inputKey:{}}),
 setHome:()=>{}, apps:async()=>[],tvApps:async()=>[],accounts:()=>({stremio:{},trakt:{},jellyfin:{},plex:{},tmdb:{}}),version:'0.25.0',elevated:()=>true });
}
test('QML About uses the updater controller and opens the real native updates page', async()=>{
 let automatic=true, actions=[];
 const u={state:()=>({automatic}),act:async body=>{actions.push(body);automatic=body.enabled;}};
 const s=settings(u), about=(await s.state()).pages.find(p=>p.id==='about');
 assert.strictEqual(about.rows.find(r=>r.id==='checkUpdates').page,'updates');
 assert.strictEqual(about.rows.find(r=>r.id==='automaticUpdates').value,'On');
 await s.act('automaticUpdates',1);
 assert.deepStrictEqual(actions,[{action:'automatic',enabled:false}]);
 assert.strictEqual((await s.state()).pages.find(p=>p.id==='about').rows.find(r=>r.id==='automaticUpdates').value,'Off');
});
test('QML About fallback still works without an updater dependency',async()=>{
 const about=(await settings().state()).pages.find(p=>p.id==='about');
 assert.deepStrictEqual(about.rows.map(r=>r.id),['remove']);
});
