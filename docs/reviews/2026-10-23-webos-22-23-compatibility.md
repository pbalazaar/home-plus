# webOS 22/23 backport validation

Base: `9c8d096898987a31391df56beb94d39a44f1ca92` (`origin/main` when this worktree was created).
Branch: `compat/webos-22-23`. No TV installation or publication was performed.

## Changes

- Replaced optional calls in the main service so Node 12 can parse it.
- Added `service/remove.js` to preserve forced file/tree cleanup on Node 12, using modern `fs.rm` where available. Symlinks are unlinked rather than followed. The synchronous fallback walks directories explicitly, avoiding Node 12's recursive `rmdirSync` hang on permission errors.
- Handled upstream image aborts before Node 12's subsequent `end` can finish a truncated downstream response. Existing tests caught this runtime difference.
- Added target-runtime syntax and real-filesystem tests, and extended CI to LG's five Node versions. The runner remains Node 22; service subprocesses and removal fixtures run on each selected target. Unit tests run on the runner; webOS/Luna and network APIs remain mocked.

## Runtime results

Exact Linux ARM64 Node distributions were downloaded from nodejs.org and checked against their published SHA-256 sums. Runtime checks ran in the existing `node:22-slim` container with:

```sh
HOMEPLUS_SERVICE_NODE=/src/build/runtimes/node-v<VERSION>-linux-arm64/bin/node node --test test/*.test.js
```

| webOS | Target Node | Passed | Failed | Existing skips |
|---|---|---:|---:|---:|
| 22 | 12.21.0 | 155 | 0 | 2 |
| 23 | 12.22.2 | 155 | 0 | 2 |
| 24 | 16.19.1 | 155 | 0 | 2 |
| 25 | 16.20.2 | 155 | 0 | 2 |
| 26 | 20.12.2 | 155 | 0 | 2 |

The pre-change baseline had 150 passed, zero failed and two skips. The new Node 12 syntax check failed on `process.getuid?.()` before changes. After that and removal were backported, two existing image-proxy tests failed on Node 12; both passed after the abort fix. The five new runtime tests also passed on the Node 22 runner itself.

The two skips are unchanged shell/curl tests: `node:22-slim` has no curl. Both passed separately in the build container, which has curl (Node 18 runner, two passed, no skips).

Independent review found a Node 12 `rmdirSync({recursive:true})` permission-error hang; an unprivileged child reproduced it with a 15-second timeout before the explicit-traversal fix. Both sync/async permission-error tests pass afterwards. The tests require the Node 12 fallback to propagate permission errors, while preserving the native API behavior of newer runtimes (Node 16's native synchronous removal can return without deleting a protected tree). No review findings remain open.

## Package and native imports

`tools/docker-build.sh` built the native launcher with the repository's pinned toolchain without compiler warnings. After the final service change, `node tools/build.mjs` repackaged that binary, and `node tools/check-package.mjs` passed (76 entries).

Package: `dist/com.github.pbalazaar.homeplus_0.18.0_all.ipk` (local development build; version intentionally unchanged).
SHA-256: `ffe3f92c42fab72dc1861020c30a213623ce57cbbc7cedd8f562ffa0c62d04d5`.

[webOSbrew's verifier](https://github.com/webosbrew/dev-toolbox-cli), release `v20261017-2d5a7b6`, commit `2d5a7b6881850d825aa7fdc6c0b5e53c91dc40d3`, checked the exact package:

```sh
webosbrew-ipk-verify --fw-releases '>=7.0, <12.0' --format markdown --details dist/com.github.pbalazaar.homeplus_0.18.0_all.ipk
```

Native library/symbol checks and service syntax/runtime-API checks passed for all available datasets in that range: internal releases **7.4.0, 8.3.0, 9.2.0, 10.2.0 and 11.2.0**. These results apply to the inspected datasets, not every firmware revision. Raw logs are in the worktree's ignored `build/node-*.tap`, `build/package.log` and `build/compatibility.md`.

## Remaining device validation

The documented real-TV baseline remains LG C3 on webOS 24/internal 9.2.1. This backport was not installed on that TV or on webOS 22/23 hardware. Native import checks and mocked API tests cannot certify LG's private Luna calls, proprietary compositor/QML APIs, input-hook behavior, audio, frame pacing or reboot persistence. Test startup/elevation, Home/Settings/Input keys, Continue Watching, the over-app Control Center, available TV controls, reboot and removal on rooted webOS 22/23 before advertising them as device-tested.

No native rendering or QML source changed, so this change did not require new screenshot comparisons.
