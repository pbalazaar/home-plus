// Minimal stand-in for webOS's webos-service module (preview harness only).
const { EventEmitter } = require('events');
module.exports = class Service {
  constructor() { this.activityManager = { create() {} }; }
  register() {}
  subscribe(uri) { // the app in front: HOMEPLUS_PREVIEW_FOREGROUND, once the service asks
    const sub = new EventEmitter();
    const app = process.env.HOMEPLUS_PREVIEW_FOREGROUND;
    if (uri.endsWith('/getForegroundAppInfo')) this.front = sub;
    if (app && uri.endsWith('/getForegroundAppInfo')) setImmediate(() => sub.emit('response', { payload: { appId: app } }));
    // tests: HOMEPLUS_PREVIEW_SUBSCRIPTIONS, what subscriptions say and when: {"<method>": [[ms, payload], ...]}, the
    // ms from when the service subscribed (each is in HOMEPLUS_PREVIEW_LUNA_LOG too, as it comes)
    const script = JSON.parse(process.env.HOMEPLUS_PREVIEW_SUBSCRIPTIONS || '{}')[uri.slice(uri.lastIndexOf('/') + 1)] || [];
    for (const [ms, payload] of script) {
      setTimeout(() => {
        if (process.env.HOMEPLUS_PREVIEW_LUNA_LOG) {
          require('fs').appendFileSync(process.env.HOMEPLUS_PREVIEW_LUNA_LOG, JSON.stringify({ uri, said: payload, t: Date.now() }) + '\n');
        }
        sub.emit('response', { payload });
      }, ms);
    }
    return sub;
  }
  call(uri, params, cb) {
    if (process.env.HOMEPLUS_PREVIEW_LUNA_LOG) { // tests: every call, one JSON line each
      require('fs').appendFileSync(process.env.HOMEPLUS_PREVIEW_LUNA_LOG, JSON.stringify({ uri, params, t: Date.now() }) + '\n');
    }
    // tests: services that never answer, the calls to them matching this regular expression
    if (process.env.HOMEPLUS_PREVIEW_LUNA_SILENT && new RegExp(process.env.HOMEPLUS_PREVIEW_LUNA_SILENT).test(uri)) return;
    if (uri.endsWith('/launch')) {
      // tests: HOMEPLUS_PREVIEW_LAUNCH_REFUSED, an app that won't open; HOMEPLUS_PREVIEW_LAUNCH_FRONT, an app that
      // opens comes to the front (a test can leave one by opening another)
      if (params.id && params.id === process.env.HOMEPLUS_PREVIEW_LAUNCH_REFUSED) return cb({ payload: { returnValue: false, errorText: 'refused' } });
      if (process.env.HOMEPLUS_PREVIEW_LAUNCH_FRONT && this.front) setImmediate(() => this.front.emit('response', { payload: { appId: params.id } }));
      return cb({ payload: { returnValue: true } });
    }
    const fake = global.__stubLuna && global.__stubLuna(uri, params); // the harness's stand-in TV
    if (fake && typeof fake.then === 'function') return void fake.then((payload) => cb({ payload })); // (later, or never)
    if (fake) return cb({ payload: fake });
    cb({ payload: { returnValue: true, apps: global.__stubApps || [] } });
  }
};
