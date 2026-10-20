# Home+ — notes for Claude

tvOS-style launcher for a rooted LG C3 (webOS 23.20.59, kernel aarch64, **32-bit ARM userland**,
SDL2 2.0.14, GLES2/EGL/Wayland). Owner's TV: `192.168.1.50`, root SSH via Homebrew Channel
(`ssh lgtv` if the key/alias from the README is set up; otherwise `root@192.168.1.50`).
PicCap and HyperHDR run on the TV, so keep GPU/CPU use low.

## Pieces
- One package, `com.github.pmiraka.homeplus` (launcher, service, boot script). The old
  `com.atvlauncher.*` ids are gone; the service still skips them in app lists.
- `service/` — Node 16 webOS service `com.github.pmiraka.homeplus.service`, elevated with Homebrew
  Channel's `elevateService` (the launcher asks; see First run). Luna methods + a local HTTP API on `127.0.0.1:47823` (see README).
  Also: Home button takeover (`watch` mode, and `redirect` = no-flash bind mount over LG home's
  start page, approved by the owner), settings apps, recents, and Continue Watching from
  Stremio + Trakt (`service.js`, `trakt.js`), Jellyfin (`jellyfin.js`) and Plex (`plex.js`), and what's popular
  on streaming apps from TMDB (`tmdb.js`, key via `tmdbSetup`). The launcher shows each as a Top Shelf
  carousel (one title at a time, turning every 10 s unless Settings keeps it still; Up then Left/Right steps through).
  The Control Center (`control.js`, `/api/control`, luna `control`) uses tvpower (`power/powerOff`,
  `power/turnOffScreen` with `standbyMode: "active"`: "passive" can't bring the picture back), settingsservice
  (`sound/soundOutput`, `time/sleepTimer`) and eim (inputs). Sign-ins
  are codes/QR codes approved on another device (Stremio's link.stremio.com, Jellyfin Quick Connect,
  Plex's PIN at plex.tv/link). `tools/native-preview/harness.js` mocks all three (`HOMEPLUS_PREVIEW_MEDIA`)
  and TMDB (`HOMEPLUS_PREVIEW_TMDB`). LG's generic backgrounds (`*default_background*`, `*_preview.png`)
  don't count as artwork: those apps get the icon glow.
  Logs to `/tmp/homeplus-service.log` (the native app logs there too, via `POST /api/log`).
- `native/` — the launcher (`com.github.pmiraka.homeplus`): C, SDL2 + our own GLES2 renderer
  (`src/render.c`: Liquid Glass shader, SDF shapes, Kawase blur; GL loaded at run time),
  `src/sound.c` (synthesised focus sounds), OLED idle stages and pixel orbit in `main.c`.
  Build only against the vendored SDL **2.0.14** headers in `third_party/SDL2` — newer SDL
  functions don't exist on the TV. Design: `docs/plans/2026-10-20-liquid-glass-design.md`.

## Building
- Native for the TV needs the Linux toolchain `openlgtv/buildroot-nc4`
  (`arm-webos-linux-gnueabi_sdk-buildroot`, from its GitHub releases): `make -C native webos WEBOS_SDK=...`.
  It doesn't run on macOS. On an Apple Silicon Mac, use the `-aarch64` SDK tarball in a
  `linux/arm64` Debian container with `make` and `nodejs` (run `relocate-sdk.sh` once after unpacking).
- Easiest anywhere (macOS too): `tools/docker-build.sh` → `dist/*.ipk` (toolchain release
  `2026.08-webos`, Intel or Apple Silicon). Without Docker: `WEBOS_SDK=... tools/build-release.sh`.
- Packages: `node tools/build.mjs` → `dist/com.github.pmiraka.homeplus_<version>_all.ipk` (one
  package: launcher, service, boot script, licence notices). Versions in `native/appinfo.json` and
  `service/package.json` must match.
- CI (`.github/workflows/build.yml`) builds every push; pushing a `v*` tag publishes a release.
- Desktop preview (Linux): `tools/native-preview/run.sh` → `preview/native-*.png`
  (`HOMEPLUS_PREVIEW_OUT` for another folder). On a Mac: `tools/native-preview/Dockerfile` (Debian with
  `libsdl2-dev xvfb xauth libgles2 libegl1 libgl1-mesa-dri python3-pil nodejs`; the Makefile finds the
  arm64 libSDL2 by itself). The harness's service believes it's root unless `HOMEPLUS_PREVIEW_UNROOTED`.
- Tests (`tools/test.sh`, CI: `.github/workflows/test.yml`): the service's unit tests and HTTP API
  tests in `test/` (Node's test runner; `node --test test/*.test.js`), every preview scene rendered and
  checked (`screens`), the .ipk built and checked (`package`: `tools/check-package.mjs` — contents,
  versions, modes, 32-bit ARM, only SDL 2.0.14 functions). Service modules read their mock URLs from env
  at load (`HOMEPLUS_TMDB`, `HOMEPLUS_PLEX_TV`, ...), so tests load them with `freshRequire`;
  `HOMEPLUS_CONFIG_DIR` and `HOMEPLUS_PORT` let the service run without root, on another port.
  `HOMEPLUS_SERVICE_NODE` runs the API tests' service on another Node (CI: 16, as on the TV).
  `tools/native-preview/glass-test.sh <art.jpg> <out.png>` renders a glass test card.
- Docker on a Mac can see a file before it's fully written: if a build reports
  "expected ... at end of input", just run it again.

## Installing on the TV
Copy the `.ipk`s to `/tmp` on the TV, then for each:
`luna-send -w 60000 -i luna://com.webos.appInstallService/dev/install '{"id":"com.ares.defaultName","ipkUrl":"/tmp/<file>","subscribe":true}'`
(stop it after ~20 s). `tools/install.sh` does all of this from a Mac or Linux machine. Opening
Home+ then re-elevates the service by itself; by hand it's
`elevate-service com.github.pmiraka.homeplus.service`.
Before installing, close the apps and stop the old service. **Never `pkill -f` a pattern that
appears in your own SSH command line** — it kills the session. Use:
`for p in $(pgrep -f homeplus.service); do [ "$p" != "$$" ] && kill "$p"; done`
- The TV has no `timeout` command, which `tools/install.sh` relies on.
- `luna-send` prints nothing unless it has a terminal: use `ssh -tt`. The install subscription
  never exits by itself. Run it in the background, wait until the installed file (e.g.
  `/media/developer/apps/usr/palm/applications/com.github.pmiraka.homeplus/homeplus`) matches the
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
- Once root, the service copies `init.d/50-homeplus` from its package into
  `/var/lib/webosbrew/init.d/` (and updates it when the package changes).
- The Home button is left alone (`homeMode` "off") until the launcher has asked
  "Use Home+ as Your Home Screen?" once (`homeAsked` in config.json). Settings has the switch.
- Settings → Remove Home+ (`POST /api/uninstall`): unmounts the no-flash page, deletes the boot
  script, lginputhook binds that launch Home+, `/var/lib/homeplus` and the elevation files, then
  `appInstallService/dev/remove`s the package.
- Preview modes: `HOMEPLUS_PREVIEW_FRESH=1` (question not answered), `HOMEPLUS_PREVIEW_UNROOTED=1`
  (unelevated service; with no luna-send on the host this shows the Homebrew Channel screen).

## Known TV quirks
- LG's SDL puts the *scancode* in `keysym.sym` (Right arrow arrives as 79); see `remote_key()`.
  LG's own codes come the same way, with `scancode=1`: Back 482 (seen), pointer show/hide 484/485
  (sent when the pointer wakes or sleeps, not real presses), Red/Green/Yellow/Blue 486–489 (from
  LG's `SDL_webOS.h`; not seen yet). Settings and Input never reach apps. Keys Home+ doesn't act on
  are logged as `unknown key sym=...`.
- With the Magic Remote cursor showing, OK arrives as a mouse click at the cursor. `main.c` keeps
  an input mode: arrow/colour/Back keys (and pointer hide, 485) hand control to the keys, and a
  click then presses the keys' focus; the pointer takes over again after 40 px of real travel
  (`PTR_TAKE_PX`), so tremor doesn't. Not yet confirmed whether LG also sends a Return key with that click
  (if it does, `ok_down` dedupes them).
- No-flash mode can't work on the C3: LG's home is a native app (`/usr/bin/com.webos.app.home`),
  so the service falls back to `watch` and reports `noFlashAvailable: false`.
- No flash on the C3 comes from lginputhook (`org.webosbrew.inputhook` 1.5.1; 1.4.0 couldn't
  inject on webOS 23). `/home/root/.config/lginputhook/keybinds.json` binds Home to
  `{"773":{"action":"launch","id":"com.github.pmiraka.homeplus"}}`, so LG's home never starts.
  Binding keys are the hook log's `uinput_code`, not `keyid`
  (`/tmp/lginput-hook-lginput2.log`). The hook injects once per boot, guarded by the `/tmp/inputhook`
  marker; after updating it, delete the marker and restart its service, or reboot.
- Developer-installed web apps can't read other apps' files; icons are served by the service.
- Stremio's LG app (io.strem.tv 1.13.3) doesn't open titles from launch params (10+ formats
  tested, cold and warm). Its files are encrypted LG Store content; don't try to work around
  that. Continue Watching opens Stremio and the service shows a system toast
  (`com.webos.notification/createToast`) naming the title.

## Open items
- Confirm the colour keys on a remote that has them.
