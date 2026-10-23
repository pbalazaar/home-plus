# Contributing to Home+

Thanks for helping with Home+. Most people install a release: the `.ipk` on the [Releases](../../releases) page. This page is for building Home+ yourself, testing a change and sending it in.

## The pieces

- `native/`: the launcher, in C. SDL2 for the window and the remote, and Home+'s own OpenGL ES 2 renderer (`src/render.c`).
- `service/`: the background service, in Node.js. It runs as root on the TV, lists and opens apps, and talks to Stremio, Trakt, Jellyfin, Plex and TMDB. The launcher reaches it over a local HTTP API.
- `init.d/`: the boot script.
- `tools/`: building, packaging and installing, and the desktop preview (`tools/native-preview/`), which runs the real service on a stand-in for webOS and renders the launcher on a Linux desktop.
- `test/`: the service's tests.

What the TV allows:

- **The launcher** is a 32-bit ARM program that uses the TV's own SDL, version 2.0.14. It's compiled against SDL 2.0.14's headers (`native/third_party/SDL2`), and the package check turns away a launcher that calls an SDL function the TV doesn't have. LG's additions to SDL are looked up while running, the way `input.c` finds `SDL_webOSCursorVisibility`.
- **The service** targets Node 12.21.0 and newer (webOS 22 and later), so optional chaining and newer APIs such as `fetch` mustn't be required. `service/remove.js` handles file and tree removal on Node 12 too. It has no npm dependencies: its folder is copied to the TV as it is.
- **The GPU and CPU** are shared with the TV's own apps, and often with PicCap and HyperHDR. Keep the launcher's use of them low.

## Building

The launcher is built with the webOS TV toolchain, [openlgtv/buildroot-nc4](https://github.com/openlgtv/buildroot-nc4), which runs on Linux. Docker brings it to macOS and Windows.

**macOS or Linux, with Docker** (Intel or Apple Silicon):

```sh
tools/docker-build.sh       # -> dist/com.github.pbalazaar.homeplus_<version>_all.ipk
```

**Linux, without Docker:** download `arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz` (or `-aarch64.tar.gz` on an ARM machine) from the toolchain's [releases](https://github.com/openlgtv/buildroot-nc4/releases). `docker/Dockerfile` names the release Home+ uses, and `docker/toolchain.sha256` has its files' checksums (`sha256sum -c`). Unpack it, run its `relocate-sdk.sh` once, then, with `make` and Node 16 or newer:

```sh
WEBOS_SDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot tools/build-release.sh
```

**Windows:** use WSL 2, which runs Linux inside Windows. The toolchain doesn't run on Windows itself.

1. In PowerShell, run `wsl --install`. It installs Ubuntu. Restart when it asks.
2. In Ubuntu, run `sudo apt install git make nodejs python3`.
3. Clone Home+ into your Ubuntu home folder (`~`), not under `/mnt/c`. It's much faster there, and the files keep their Linux permissions.
4. Build as on Linux, either way:
   - with Docker: install Docker Desktop and turn on its WSL integration for Ubuntu (Settings → Resources → WSL integration);
   - with the toolchain, without Docker.

## Trying it on a TV

- **From macOS, Linux or WSL:** `tools/install.sh <tv-ip>` copies the package to the TV over SSH (as root, which the Homebrew Channel provides), installs it, sets up the service's root access and opens Home+.
- **From any computer:** install `dist/*.ipk` with [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop), then open Home+. It sets up its root access on its first start.

The logs are in the README's [Troubleshooting](README.md#troubleshooting).

## Testing

```sh
tools/test.sh               # the service's tests: unit tests and its HTTP API (seconds)
tools/test.sh screens       # renders every launcher screen and checks it (about 2 minutes)
tools/test.sh qml           # loads the Control Center's QML and drives it, as LG's compositor would
tools/test.sh package       # builds the .ipk and checks it
tools/test.sh all           # all four
```

- **The service's tests** need Node 18 or newer. In PowerShell, run them with `node --test "test/*.test.js"` (Node 22 or newer). `HOMEPLUS_SERVICE_NODE=/path/to/node node --test test/*.test.js` runs service subprocesses and cleanup checks on an older Node while keeping the test runner on the newer one; it also checks that every shipped service module parses on the target runtime. Everything the service talks to (TMDB, Jellyfin, Plex, Stremio, the TV) is mocked, so they need no network and no TV.
- **The Control Center over other apps** is QML that LG's compositor runs (`native/qml`), and the TV keeps no log of its errors. `tools/test.sh qml` loads it on Qt's own QML runtime, with stand-ins for LG's modules (`tools/qml-check/stubs`), opens it with sample controls and goes through it as the remote would; any QML error fails it.
- **The screens** are rendered on a Linux desktop with Mesa. Elsewhere, `tools/test.sh` renders them in Docker. The check makes sure that every screen was drawn, at 1920×1080, that it isn't blank, and that it isn't just the home screen again. Look at them as well: they're in `build/screens/`.
  - For a new screen, add a `shot` line to `tools/native-preview/run.sh`: the keys to press, and when to take the picture.
  - `tools/native-preview/run.sh` on its own updates the screenshots in `preview/`.
- **The package check** makes sure that:
  - the `.ipk` has everything the TV needs;
  - the versions agree;
  - nothing private ships (a local config can hold sign-ins);
  - the launcher is 32-bit ARM and calls only SDL functions the TV has.

CI runs all of it on every pull request, except the screens:

- the service's tests, on Linux with the service on the Node versions in webOS 22–26, and on Windows;
- the launcher's own tests (`native/test/`), and the Control Center's QML, loaded and driven;
- the package.

The screens are rendered on a desktop instead (`tools/test.sh screens`): in CI they took most of the run, and Mesa under Xvfb isn't the TV's GPU.

**A real TV** is the only way to check the luna calls, the remote, how the launcher looks on the panel and how smoothly it runs. If you changed one of those and couldn't try it on a TV, say so in the pull request.

## Pull requests

- Keep each pull request to one change, and say what it changes and why.
- Match the code around it: its names and its comments. C is gnu11 and builds without warnings. The service's JavaScript must run on Node 12.21.0 and newer.
- Add or update the service's tests, or a preview scene for a new screen.
- For a change you can see, add before and after screenshots.
- Leave the version alone: it changes when a release is made.

## Releases

For maintainers:

1. Set the new version in both `native/appinfo.json` and `service/package.json`, and commit.
2. Tag the commit and push the tag:

   ```sh
   git tag v0.11.0
   git push origin v0.11.0
   ```

CI builds the package, checks it and runs every test; once all of that has passed, it publishes a release with the `.ipk` attached.
