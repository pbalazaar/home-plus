# How Home+ works

One package, `com.github.pbalazaar.homeplus`, holds three things:
- **The launcher** (`native/`): C with SDL2 and OpenGL ES 2, using the TV's own libraries. Our renderer ([`native/src/render.c`](../native/src/render.c)) draws the glass, shapes and shadows with shaders. A frame is a few dozen quads, and glass works on a half-resolution copy of the screen. Animations are springs, and nothing is redrawn while nothing moves.
- **The service** (`service/`): a Node.js webOS service that runs as root once the Homebrew Channel elevates it. It lists, opens and closes apps and serves their icons. It also talks to Stremio, Trakt, Jellyfin and Plex, and handles the Home button.
- **The boot script** (`init.d/`): makes the Homebrew Channel re-elevate and start the service at every boot. The service links it into the Homebrew Channel's `/var/lib/webosbrew/init.d/`: a link, not a copy, so it goes with the package.

The launcher talks to the service over a local HTTP API on `127.0.0.1:47823`. It uses the webOS luna bus only to start the service and to ask the Homebrew Channel for root.

| Endpoint | What it does |
|---|---|
| `GET /api/status` | Version, whether the service is root, setup state (no token needed) |
| `GET /api/apps` | App list |
| `POST /api/launch` | Open an app |
| `POST /api/resume` | Play a Jellyfin title in its app: `{"app", "source": "jellyfin", "id", "fromStart"}`. Opens the app and answers as `/api/launch` does; the server is told what to play after that |
| `GET` / `POST /api/layout` | Tile order and hidden apps |
| `GET /api/continue` | Continue Watching from Stremio and Trakt, or with `?source=jellyfin` or `plex` |
| `GET /api/featured?app=<id>` | What's popular on a streaming app (TMDB), for its Top Shelf |
| `GET` / `POST /api/control` | The Control Center: network, sound output, sleep timer and inputs; `{action, value}` acts |
| `POST /api/overlay` | The Control Center over other apps asks for what it can't do itself: `{"event": "open", "target": "network"}`, or a menu or Settings page for Home+ to open (`"recents"`, `"accessibility"`, `"settings"`, `"general"`, `"accounts"`, `"about"`); `{"event": "open", "app"}` (an app it offered); `{"event": "recents"}` (the apps for its Recents, each icon a `file://` path); `{"event": "closed"}`, `{"event": "failed"}` |
| `POST /api/settings` | Settings over other apps (`service/settings.js`): the launcher's pages as data, for the panel to draw; `{"row", "dir"}` changes a row first |
| `GET /api/<account>/status`, `POST .../link`, `POST .../disconnect` | Accounts (`stremio`, `trakt`, `jellyfin`, `plex`): state, a sign-in code, signing out |
| `POST /api/jellyfin/server` | A Jellyfin server's address, for one Home+ can't find |
| `GET /api/system` | LG's settings apps |
| `GET /api/running`, `POST /api/close` | Recents, and closing apps |
| `POST /api/remove` | Deletes an app from the TV (`{"id": ...}`) |
| `GET` / `POST /api/home` | Home button mode; `onboarded` marks the welcome steps done |
| `GET /api/keys`, `POST .../open`, `POST .../close`, `GET .../qr` | The keys page for TMDB and Trakt, on the local network: whether it's open, its link, its QR code |
| `GET` / `POST /api/prefs` | `sounds`, `dimMinutes`, `offMinutes`, `pixelShift`, `calmColours`, `clock24`, `reduceMotion`, `reduceTransparency` |
| `POST /api/uninstall` | Remove Home+ |
| `GET /icon/<id>`, `/art/<id>`, `/img/<key>` | App icons, app artwork, and artwork and QR codes from the accounts |

```
native/     the launcher (C, SDL2 + OpenGL ES 2)
service/    the Node.js service
init.d/     the boot script
docker/     the build environment
tools/      build.mjs (packages the .ipk), install.sh, build-release.sh, docker-build.sh, native-preview/
preview/    screenshots from tools/native-preview
docs/       design notes
```

[`CLAUDE.md`](../CLAUDE.md) holds development notes, including the webOS quirks found along the way. Who makes Home+, and how: [How Home+ is built and tested](../README.md#how-home-is-built-and-tested).
