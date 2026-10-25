# Home+ — notes for Claude

tvOS-style launcher for a rooted LG C3 (webOS 9.2.1, firmware 23.20.59, kernel aarch64, **32-bit ARM userland**,
SDL2 2.0.14, GLES2/EGL/Wayland). The TV: `<tv-ip>`, root SSH via Homebrew Channel
(`ssh root@<tv-ip>`, or an alias of your own).
PicCap and HyperHDR run on the TV, so keep GPU/CPU use low.

## Pieces
- One package, `com.github.pbalazaar.homeplus` (launcher, service, boot script). The old
  `com.atvlauncher.*` ids are gone; the service still skips them in app lists. So is
  `com.github.pmiraka.homeplus`, Home+'s id up to 0.14.0 (`OLD_ID`): a new id is another app, whose
  service would take the port, so it's uninstalled first (`tools/install.sh` does it; the launcher's
  "Uninstall the Old Home+" says so when `/api/status` isn't its own service's), and LG Input Hook
  binds made for it count as Home+'s.
- `service/` — Node 8.12+ webOS service (webOS 6 and webOS 22 and later) `com.github.pbalazaar.homeplus.service`, elevated with Homebrew
  Channel's `elevateService` (the launcher asks; see First run). Luna methods + a local HTTP API on `127.0.0.1:47823` (see `docs/ARCHITECTURE.md`).
  Also: Home button takeover (`watch` mode, and `redirect` = no-flash bind mount over LG home's
  start page, approved by the owner; and an app that quits brings Home+ at once, see Known TV quirks), settings
  apps, recents, and Continue Watching from
  Stremio + Trakt (`service.js`, `trakt.js`), Jellyfin (`jellyfin.js`) and Plex (`plex.js`), and what's popular
  on streaming apps from TMDB (`tmdb.js`, key via `tmdbSetup`). The launcher shows each as a Top Shelf
  carousel (one title at a time, turning every 10 s unless Settings keeps it still; Up then Left/Right steps through).
  The Control Center (`control.js`, `/api/control`, luna `control`) uses tvpower (`power/powerOff`,
  `power/turnOffScreen` with `standbyMode: "active"`: "passive" can't bring the picture back), settingsservice
  (sound output, picture mode, and steppers over any described setting), tvReservationAgent (the sleep timer)
  and eim (inputs). Its controls are a catalogue in `control.js` plus `ambient.js` (HyperHDR's LEDs over its
  JSON-RPC on 127.0.0.1:8090, PicCap's capture over its luna service); the list on show is `controlCenter`
  in config.json (editable from the launcher: hold OK in the Control Center). Sign-ins
  are codes/QR codes approved on another device (Stremio's link.stremio.com, Jellyfin Quick Connect,
  Plex's PIN at plex.tv/link). `tools/native-preview/harness.js` mocks all three (`HOMEPLUS_PREVIEW_MEDIA`)
  and TMDB (`HOMEPLUS_PREVIEW_TMDB`). LG's generic backgrounds (`*default_background*`, `*_preview.png`)
  don't count as artwork: those apps get the icon glow.
  Logs to `/tmp/homeplus-service.log` (the native app logs there too, via `POST /api/log`).
- `native/qml/` — the Control Center over other apps: QML that LG's compositor (surface-manager) runs as a
  system UI, as it runs LG's Quick Settings, opened by the service on the Settings key when Home+ isn't in
  front (`systemUIManager/launch` with our own `main` and `windowType: _WEBOS_SYSTEM_UI_TYPE_OVERLAY`; the
  video keeps playing). `ControlCenter.qml` is the window (lifecycle, the API calls), `ControlCenterView.qml`
  the panel (Home+'s sizes, editing, lists, gallery, sheets), `Glass.qml` its glass, `Tint.qml` a glyph's
  colour, `Marquee.qml` a long name; `glyphs/` are Home+'s icons as PNGs (`tools/make-glyphs.sh`), `sounds/` its
  focus sounds (`tools/make-sounds.py`). Found on the C3:
  - The compositor keeps every QML file it has loaded, by URL, until the TV restarts: the service opens it
    from a link named for the files' time (`qml-<time> -> qml` in the app folder), so updates load.
  - It may call only `com.lge.*`, `com.palm.*` and `com.webos.*` over luna ("Not permitted to send to"
    ours), but `XMLHttpRequest` to `127.0.0.1` works: the panel uses the API, the token is in the launch params.
  - Back closes an overlay unless its window sets `_WEBOS_ACCESS_POLICY_KEYS_BACK` "true" (as LG's Quick
    Settings do); then Back arrives as key `0x1200003`. OK is `Qt.Key_Return`, a press and a release (no repeat).
  - Its glass: `com.webos.service.capture/executeOneShot` `method: VIDEO` (the video, without any graphics)
    takes ~350 ms a frame at any size, so ~3 a second; DRM'd video gives none.
  - The service takes one capture at a time (the snapshot too, and the next opening's), 3 a second at most from
    each start; a frame the same as the one on show isn't sent, and with no picture it tries less often (to 5 s).
  - The C3 has one video capture (the media resource `VT_CAPTURE`, `com.webos.media/getActivePipelines`
    shows who holds it), and PicCap's vtcapture keeps it while it captures (after a reboot, from boot): then
    VIDEO says `ERROR_NOT_INITIALIZED`, and DISPLAY, screenshots included, is black where the video is. The
    service checks each frame (`service/png.js`): black behind the panel isn't shown, and the glass is clear.
    Settings → Control Center → Blur Over Video (`ccBlurVideo` in prefs, with PicCap): pauses PicCap's capture
    while it's open (`ambient.js`). The glass has no Light/Dark: like tvOS's, it takes its look from what's behind.
  - It can't post a notification that leaves the remote to the app: an overlay keeps the keys even when it
    doesn't take them (tried), so Home+'s hints stay LG's toasts.
  - Sounds play through QtMultimedia's `SoundEffect` (`Sounds.qml`, loaded on its own in case it can't).
  - No logs: to debug, put a `Text` on screen and take a screenshot. `touch /tmp/homeplus-cc-fps` shows its
    frame rate and the last key code. A failure to load is reported to the service (`overlay failed:` in its log).
- `native/` — the launcher (`com.github.pbalazaar.homeplus`): C, SDL2 + our own GLES2 renderer
  (`src/render.c`: Liquid Glass shader, SDF shapes, Kawase blur; GL loaded at run time),
  `src/sound.c` (synthesised focus sounds). The launcher itself is a file per feature (`src/home.h` lists them,
  with the layout, types and the one global state `S` they share): OLED idle stages and pixel orbit are in
  `main.c`'s `update()`, the wait for the next stage in `idle.c`.
  Frame timing (`perf.c`), off unless `/tmp/homeplus-perf` exists in Home+'s jail
  (`/var/palm/jail/com.github.pbalazaar.homeplus/tmp/`, `HOMEPLUS_PERF=1` in the preview): each run of frames is
  then a `perf:` line in the service's log (fps, the gaps in refreshes, late frames, CPU per frame, the first dt,
  key to screen, the slowest answer between frames, how many frames drew each backdrop); `homeplus-perf-gpu` too
  waits for the GPU after each part of a frame, which says what each costs (`perf gpu:`). Each return to the
  front is a `perf: back:` line: when its window events came, come_back(), the first frame and the first with the
  Top Shelf complete and settled, with the first event's wall-clock ms (a script on the TV can line it up with the
  Home button). `perf: pictures:` says what pictures cost since the last rest (`net.c`: one decoded at a time within
  64 MB, at most 16 MB of them waiting to be taken), and `perf: memory` what textures and render targets hold
  as Home+ hides, and after 30 s hidden, when it lets go of the render targets (`hidden_idle()`; the first frame back
  makes them again). A backdrop is drawn only when what's in it changes (`draw_backdrop()`: each draw is first noted
  into a fingerprint). Back from an app opened from Home+, the home screen is as it was left, the focus on what
  opened the app (as tvOS's; it went back to the top until 0.18.0): `ready_home()` (`idle.c`), as Home+ goes, loads
  the page the Top Shelf wants there while hidden and shows it in place of the page left (`shelf_jump()`), and the
  first frame back moves every animation to its end (`S.settle`). Then it comes into focus over `BACK_MS` (700 ms,
  `back_in()`, `draw.c`): the scene's blurred, dimmed backdrop over it clears, easing out gently (not with Reduce
  Motion: it's there at once).
  Settings is two panes (`settings.c`, drawn by `draw_split()` in `sheet.c`): `CATS[]` is the sidebar (a page, a line
  about it, a glyph), `PAGE_MAIN` means the sidebar has focus and its pane shows that section's rows; rows have a
  description, and those in `settings_steps()` change with Left/Right (`settings_keydown()`, `arg` -1 steps back).
  Each move rebuilds the sheet's textures, as a page change always did.
  App tile looks (Settings → App Tiles; prefs `iconLook`, `iconTint` from `TINTS`):
  `draw_face()` (`draw.c`) draws every tile (dock, big tile, Recents), `FS_LOGO` the logo in one colour or for a
  dark tile. `make_logo()` (`tiles.c`) cuts a logo out of its icon's background, edge pixels unblended from it
  (a light icon's logo on a dark tile kept a rim of it); Default's sizes still come from the plain key.
  Build only against the vendored SDL **2.0.14** headers in `third_party/SDL2` — newer SDL
  functions don't exist on the TV. Design: `docs/plans/2026-10-20-liquid-glass-design.md`.

## Building
- Native for the TV needs the Linux toolchain `openlgtv/buildroot-nc4`
  (`arm-webos-linux-gnueabi_sdk-buildroot`, from its GitHub releases): `make -C native webos WEBOS_SDK=...`.
  It doesn't run on macOS. On an Apple Silicon Mac, use the `-aarch64` SDK tarball in a
  `linux/arm64` Debian container with `make` and `nodejs` (run `relocate-sdk.sh` once after unpacking).
- Easiest anywhere (macOS too): `tools/docker-build.sh` → `dist/*.ipk` (toolchain release
  `2026.08-webos`, Intel or Apple Silicon). Without Docker: `WEBOS_SDK=... tools/build-release.sh`.
- Packages: `node tools/build.mjs` → `dist/com.github.pbalazaar.homeplus_<version>_all.ipk` (one
  package: launcher, service, boot script, licence notices). Versions in `native/appinfo.json` and
  `service/package.json` must match.
- CI (`.github/workflows/ci.yml`) builds and tests every push; pushing a `v*` tag publishes a release
  once its build, service tests, launcher job (its own tests, the QML check) and compatibility check have
  passed. The compatibility job runs webOSbrew's `webosbrew-ipk-verify` (dev-toolbox-cli, built at
  `VERIFY_REV`, cached with its firmware data) over the package for `VERIFY_FW` (`>=5.0`: webOS 5, 2020, on;
  every generation is rootable on some firmware, cani.rootmy.tv, but webOS 4 and older have Node 0.12 or
  older). Its datasets' Node: webOS 5 and 6 8.12.0, 22 12.21.0, 23 12.22.2, 24 16.19.1, 25 16.20.2, 26 20.12.2. The
  launcher's screens aren't rendered in CI (they took most of a run, and a slow apt mirror then put it
  past its 15 minutes): render them with `tools/test.sh screens`. The toolchain is checked against
  `docker/toolchain.sha256` (change it with `SDK_RELEASE`); the actions are pinned to commits.
- Desktop preview (Linux): `tools/native-preview/run.sh` → `preview/native-*.png`
  (`HOMEPLUS_PREVIEW_OUT` for another folder). On a Mac: `tools/native-preview/Dockerfile` (Debian with
  `libsdl2-dev xvfb xauth libgles2 libegl1 libgl1-mesa-dri python3-pil nodejs`; the Makefile finds the
  arm64 libSDL2 by itself). The harness's service believes it's root unless `HOMEPLUS_PREVIEW_UNROOTED`.
- Tests (`tools/test.sh`, CI: `.github/workflows/ci.yml`): the service's unit tests and HTTP API
  tests in `test/` (Node's test runner; `node --test test/*.test.js`), the launcher's own (`native`:
  `native/test/`, `make -C native test`: `net.c` against a stand-in service, `icon.c`, `util.c`'s `media_source()`), every preview scene rendered
  and checked (`screens`, on a desktop only), the .ipk built and checked (`package`: `tools/check-package.mjs` — contents,
  versions, modes, 32-bit ARM, only SDL 2.0.14 functions). Service modules read their mock URLs from env
  at load (`HOMEPLUS_TMDB`, `HOMEPLUS_PLEX_TV`, ...), so tests load them with `freshRequire`;
  `HOMEPLUS_CONFIG_DIR` and `HOMEPLUS_PORT` let the service run without root, on another port, and
  `HOMEPLUS_APP_DIR`, `HOMEPLUS_INITD_DIR` and `HOMEPLUS_INPUTHOOK_BINDS` stand in for the app folder,
  Homebrew Channel's init.d and LG Input Hook's binds (`startService` in `test/helpers.js` starts it).
  `HOMEPLUS_SERVICE_NODE` runs the tests' service and cleanup checks on another Node (CI: webOS 5 and 6's 8.12.0
  and webOS 22–26's 12.21.0, 12.22.2, 16.19.1, 16.20.2 and 20.12.2; the test runner stays on Node 22). All
  shipped service modules are syntax-checked on that target too. Use `service/remove.js` for forced
  file/tree cleanup and `service/mkdir.js` for `mkdir -p`: Node 8 and 12 have no `fs.rm`/`fs.rmSync`, Node 8
  no `{ recursive: true }`, and neither parses optional chaining. Node 8 (the LG C1's) also lacks the global
  `URL` (require it from `url`), `request(url, options, cb)` (`requestOptions()` in `net.js`; the harness's
  mocks take `(options, cb)` too), `Object.fromEntries`, `.flat()`, `Promise.prototype.finally`, regex named
  groups and `\p{}` (`--check` misses those), and a stable `sort()` past 10 items (`service/sort.js`). The tests
  that load service modules themselves run on that target too (`runtime.test.js`, through `test/target/run.js`,
  a stand-in for the bit of `node:test` they use), so they're written for Node 8 as well (`last()` in helpers,
  `remove.rmSync`, no global `URL`).
  `HOMEPLUS_LOG` and `HOMEPLUS_RECENTS_FILE` move its files in /tmp (opened only as its own, never
  through a link: `service/tmp.js`); `HOMEPLUS_FETCH_TIMEOUT_MS` and `HOMEPLUS_LUNA_TIMEOUT_MS` shorten
  the image proxy's and luna calls' 15 s. The harness's `HOMEPLUS_PREVIEW_ART` serves the mock Stremio
  titles' pictures from a test's server, `HOMEPLUS_PREVIEW_HTTPS` sends Stremio's and Trakt's API calls to
  one, `HOMEPLUS_PREVIEW_LUNA_SILENT` (a regular expression) makes those luna calls never answer, and
  `HOMEPLUS_PREVIEW_NO_APPS` has webOS list no apps.
  `tools/native-preview/glass-test.sh <art.jpg> <out.png>` renders a glass test card.
  `tools/native-preview/compare-screens.py <before> <after>` compares two folders of preview shots, for a change
  that should draw the same: it leaves out the clock, and allows the few scenes caught mid-animation the difference two
  runs of one build show (~0.3%, and up to ~2% for `crossfade`).
- Docker on a Mac can see a file before it's fully written: if a build reports
  "expected ... at end of input", just run it again.
- The README's banner and GitHub's social preview (the picture a link to the repo shows): `tools/make-banner.py`
  → `docs/banner.jpg` and `docs/social-preview.jpg`, from `docs/screenshots/home-clear.jpg` (the desktop preview
  fed by the TV's own service through a read-only proxy, App Tiles on Clear). The social preview is uploaded by
  hand on GitHub (Settings → General → Social preview); GitHub's repo card shows only its middle three quarters.

## Installing on the TV
Copy the `.ipk`s to `/tmp` on the TV, then for each:
`luna-send -w 60000 -i luna://com.webos.appInstallService/dev/install '{"id":"com.ares.defaultName","ipkUrl":"/tmp/<file>","subscribe":true}'`
(stop it after ~20 s). `tools/install.sh` does all of this from a Mac or Linux machine. Opening
Home+ then re-elevates the service by itself; by hand it's
`elevate-service com.github.pbalazaar.homeplus.service`.
Before installing, close the apps and stop the old service. **Never `pkill -f` a pattern that
appears in your own SSH command line** — it kills the session. Use:
`for p in $(pgrep -f homeplus.service); do [ "$p" != "$$" ] && kill "$p"; done`
- The TV has no `timeout` command, which `tools/install.sh` relies on.
- `luna-send` prints nothing unless it has a terminal: use `ssh -tt`. The install subscription
  never exits by itself. Run it in the background, wait until the installed file (e.g.
  `/media/developer/apps/usr/palm/applications/com.github.pbalazaar.homeplus/homeplus`) matches the
  new build, then kill it. The install un-elevates the service; the launcher fixes that on its next start.
- Screenshot: `luna-send -n 1 luna://com.webos.service.capture/executeOneShot '{"path":"/tmp/s.png","method":"DISPLAY","format":"PNG","width":1920,"height":1080}'`.

## First run and removal
- The launcher runs in the app sandbox, where `luna-send` can't reach other services (seen on the
  C3: the wake ping and Homebrew Channel's elevateService both failed). `native/src/luna.c` calls
  the bus itself via the TV's libluna-service2 (dlopen) with an anonymous handle (the app id is
  taken by LG's runtime); luna-send is only the fallback. Verified on the C3 (0.8.1): wake,
  elevateService and the restart all go through. Every call is logged to `/tmp/homeplus-native.log`
  (inside the app's jail; `find / -name homeplus-native.log`).
- Homebrew Channel doesn't elevate other apps' services on install (only its own), and every
  install un-elevates ours, so the launcher's elevation step runs after each update too.
- The launcher starts with `GET /api/status` (no token needed). No answer: it wakes the service
  (a luna ping) and retries ("Setting Up Home+"; after ~24 s "Home+ Couldn't Start"). Answer but not
  root: it calls Homebrew Channel's `elevateService` with the service id, then `POST /api/restart`
  (refused once root) so the next start runs elevated. Errors: "Homebrew Channel Needed" (luna says
  the service doesn't exist) or "Couldn't Get Permission". Every failure screen has Try Again.
- Once root, the service links `/var/lib/webosbrew/init.d/50-homeplus` to `init.d/50-homeplus` in its
  package (replacing the copy 0.11.1 and older made): Homebrew Channel's publishing rules want a link,
  so the script goes with the package. The C3's run-parts (busybox 1.35) skips the dead link left
  after a plain uninstall with one stderr line and runs the rest (exit 0; tried on the TV). The
  script logs the last boot to `/var/lib/webosbrew/homeplus-boot.log` (not /tmp: the rules ask for it
  there) and stops if Home+'s service isn't installed.
- The Home button is left alone (`homeMode` "off") until the launcher has asked
  "Use Home+ as Your Home Screen?" once (`homeAsked` in config.json). Settings has the switch.
- Once set up, the first open shows the welcome steps (`welcome_page()`, `setup.c`; `PAGE_WELCOME`): Welcome
  (Get Started or Skip), the Home question, the remote's buttons (with LG Input Hook), Continue Watching sign-ins
  (apps on the TV), App Tiles, then a tip or two. Their choices are Settings' own actions. Done or skipped, they're
  `onboarded` in config.json (`POST /api/home {"onboarded":true}`; the Home answer sends `false` with it);
  without the field, `homeAsked` counts, so an update from before them doesn't show them.
- TMDB's key and Trakt's Client ID/Secret are pasted on a phone (`service/keys.js`): `POST /api/keys/open` serves a
  form on the LAN (port 0, path of 128 random bits, closed on save, on the sheet's Cancel/Back or after 10 min) and
  the launcher shows its link as a QR code (`service/qr.js`, our own encoder: byte mode, level M, versions 1–10;
  `/api/keys/qr`). A save emits `keys` on `/api/events`. `HOMEPLUS_KEYS_HOST` names the address in the link
  (tests, the preview).
- Settings → Remove Home+ (`POST /api/uninstall`): unmounts the no-flash page, deletes the boot
  script and its log, lginputhook binds that launch Home+, `/var/lib/homeplus` and the elevation files,
  then `appInstallService/dev/remove`s the package. A plain uninstall (Homebrew Channel) runs none of
  that: what's left must be harmless (the dead link, the binds' fallbacks below); `/var/lib/homeplus` stays.
- Preview modes: `HOMEPLUS_PREVIEW_FRESH=1` (the welcome steps), `HOMEPLUS_PREVIEW_UNROOTED=1`
  (unelevated service; with no luna-send on the host this shows the Homebrew Channel screen),
  `HOMEPLUS_PREVIEW_OLD=1` (the old Home+'s service answers: "Uninstall the Old Home+").

## Known TV quirks
- Drawing on the C3 (Mali-G52 MC3, r46p0; measured with `perf.c`): the screen refreshes at 60 Hz, but after
  50 Hz video (an IPTV channel) it stays at 50 Hz, frames 20 ms apart, and SDL still says 60. `SDL_GL_SwapWindow`
  returns at once and the wait for a refresh is in the frame's first draw to the screen (a free buffer), so
  the CPU's part of a frame (1.5 ms, 3.5 with an overlay) hides in "draw". The driver has
  GL_EXT_disjoint_timer_query but times every frame at 0; the GPU's busy share is
  `/sys/kernel/debug/mali0/dvfs_utilization` (busy and idle time so far: read it twice). GPU per frame (the
  probe, so a little high): each half-size backdrop with its five blur passes ~4 ms when it's drawn, the home
  screen at full size ~5, a sheet ~5.5. HyperHDR (with PicCap) keeps one of the three cores busy.
- LG's SDL puts the *scancode* in `keysym.sym` (Right arrow arrives as 79); see `remote_key()`.
  LG's own codes come the same way, with `scancode=1`: Back 482 (seen), pointer show/hide 484/485
  (sent when the pointer wakes or sleeps, not real presses), Red/Green/Yellow/Blue 486–489 (from
  LG's `SDL_webOS.h`; not seen yet). Settings and Input never reach apps. Keys Home+ doesn't act on
  are logged as `unknown key sym=...`.
- With the Magic Remote cursor showing, OK arrives as a mouse click at the cursor. `input.c` keeps
  an input mode: arrow/colour/Back keys (and pointer hide, 485) hand control to the keys, and a
  click then presses the keys' focus; the pointer takes over again after 40 px of real travel
  (`PTR_TAKE_PX`), so tremor doesn't. Not yet confirmed whether LG also sends a Return key with that click
  (if it does, `ok_down` dedupes them).
- No-flash mode can't work on the C3: LG's home is a native app (`/usr/bin/com.webos.app.home`),
  so the service falls back to `watch` and reports `noFlashAvailable: false`.
- When an app quits (its Exit, Back on its first screen, SAM's `closeByAppId`), SAM says `close` and `stop`
  (`getAppLifeEvents`), the foreground goes empty ~80 ms later, the screen is black, and 1.0 s after that SAM's
  last-app handler launches its idle app (settingsservice `general`/`lastAppHandlerPolicy` "idleApp", configd
  `profile.idleApp` = LG's home): LG's home was in front ~120 ms before the watch's Home+ replaced it. The
  service launches Home+ as soon as the app in front has quit and nothing's there (50 ms left for another app
  to say it's coming); SAM then finds an app in front and launches nothing (on the C3: LG's home in none of 26
  exits; Home+'s first frame ~0.23 s after the exit, was ~1.33 s). Not for an app sent to the background:
  LG's compositor brings back the most recent card on a minimize (platformBack), and after a window that sets
  `_WEBOS_LAUNCH_PREV_APP_AFTER_CLOSING` (`/usr/lib/qt5/qml/WebOSCompositor/controllers/StarfishFullscreenController.qml`).
- Hidden, the launcher pumps SDL every 100 ms, so SAM's relaunch (the Home button) waited 0–100 ms. When SAM
  says it's launching Home+ while another app is in front, the service sends the launcher `wake` (`/api/events`),
  and it pumps every 10 ms for 300 ms (`keys_until`).
- No flash on the C3 comes from lginputhook (`org.webosbrew.inputhook` 1.5.1; 1.4.0 couldn't
  inject on webOS 23). `/home/root/.config/lginputhook/keybinds.json` binds Home (773) to Home+, so
  LG's home never starts. Binding keys are the hook log's `uinput_code`, not `keyid`
  (`/tmp/lginput-hook-lginput2.log`). The hook injects once per boot, guarded by the `/tmp/inputhook`
  marker; after updating it, delete the marker and restart its service, or reboot. It rereads
  keybinds.json whenever the file changes ("Keybinds reloaded due to config file change").
- Home+'s binds outlive a plain uninstall, so they're `exec` binds that check for Home+ and otherwise
  do the key's LG job: Home `[ -f <app>/appinfo.json ] && exec luna-send ... launch Home+; exec
  luna-send ... launch com.webos.app.home`, Settings settings-key.sh or LG's Quick Settings, Input
  input-key.sh or LG's Input picker (`OUR_BINDS`).
  They test a packaged file, not the folder: the app folder also holds the service's `api-token`, which
  isn't the package's. At start and whenever keybinds.json changes, the service upgrades the old
  forms (`OLD_BINDS`): a `launch` of Home+ on 773 (LG Input Hook's "Launch App") and 0.11's
  `sh settings-key.sh`. Others' binds are never touched. Speed: the TV's 1.5.1 hook runs both `launch`
  and `exec` binds through `/bin/sh` (popen), so `exec` costs nothing extra; current inputhookpp
  spawns luna-send directly for `launch`, and the shell adds ~1.5 ms (measured on the C3).
- Developer-installed web apps can't read other apps' files; icons are served by the service.
- Stremio's LG app (io.strem.tv 1.13.3) doesn't open titles from launch params (10+ formats
  tested, cold and warm). Its files are encrypted LG Store content; don't try to work around
  that. Continue Watching opens Stremio and the service shows a system toast
  (`com.webos.notification/createToast`) naming the title.
- Stremio's streaming server is a webOS service of its own (`io.strem.tv.server`, ~30 MB) that keeps
  running after Stremio closes, and webOS's memory manager closes apps, not services: `stremio-server.js`
  stops it once Stremio has been closed for 10 s (Stremio starts it again when it opens).
- webOS runs apps nobody opened (preloaded, or started again in the background after a close), so
  `applicationManager/running` isn't Recents. The service lists only running apps that have been in
  front (`lastUsed`, kept in `/tmp/homeplus-recents.json`); closing or quitting forgets an app.
- LG's Settings app (`com.palm.app.settings`) opens on Picture without params and has no network
  page. Network is `com.webos.app.firstuse-overlay` with `{"target":"network"}` (what LG's Quick
  Settings use); Settings pages are `{"target":"picture"|"sound"|"general"|...}`.
- The remote's Settings key is lginputhook code 139. Its actions are pass, ignore, replace, exec
  (`/bin/sh`, root) and launch; none is conditional and exec/launch swallow the key. LG's Quick
  Settings open with `systemUIManager/launch {"appId":"com.webos.app.quicksettings"}`.
- The Input key is code 241 (KEY_VIDEO_NEXT; seen on the C3's Magic Remote). Without a bind LG's
  compositor opens its Input picker, `systemUIManager/launch {"appId":"com.webos.app.quickinputpicker"}`.
  Bound (Settings -> Remote -> Input Button), it opens the Control Center on its inputs alone, in Home+
  (the `inputs` event) or over the app in front (the overlay with `{list: "inputs"}`); pressed again
  over an open panel, the service asks that panel (`{again: "input"}`) rather than open a new one, so
  its frames go on.
- What LG's compositor itself draws, and switches for it, are in its config
  (`/etc/configd/layers/base/com.webos.surfacemanager.json`, read live with
  `com.webos.service.config/getConfigs`): its system UIs (Quick Settings, the Input picker, the on-screen
  remote, "nudge" banners) and `volumeOSD` (its volume bar), `disableSourcePicker`, `disableQuickSettings`.
  Its key handlers are `/usr/lib/qt5/qml/KeyFilters/appLaunch.js` and `systemUi.js`.
- `listApps` gives `removable` and `systemApp`. LG's home deletes store apps with
  `applicationManager/removeLaunchPoint {"launchPointId":"<id>_default"}`; dev apps use
  `appInstallService/dev/remove`.
- Picture mode is `settingsservice` `picture`/`pictureMode`, read and set without a `dimension` for
  the current input and range; `getSystemSettingDesc` lists the modes (`active && visible`).
- The sleep timer that turns the TV off is `com.webos.service.tvReservationAgent`
  `timer/sleep/add {"sleepTimeMinute":N}` (0 = off) and `timer/sleep/getRemainTime`; the
  `time/sleepTimer` setting doesn't start it. With the output on the TV's speakers, webOS reports no
  ARC device (a soundbar asleep, `arcDevice: false`), so Sound always offers HDMI ARC;
  `sound/eArcSupport` says whether it's eARC.

## Open items
- Confirm the colour keys on a remote that has them.
