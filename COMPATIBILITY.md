# Compatibility

The TVs Home+ has been tried on, from reports by the people who tried them. A report that something doesn't work is as useful as one that does.

**Tried it on a TV that isn't here, or a different firmware?** [Send a compatibility report](https://github.com/pbalazaar/home-plus/issues/new?template=compatibility.yml). It takes a few minutes, and it's added to this list with your name.

| TV | webOS (firmware) | Home+ | Works | Doesn't work, or not tried | Report |
|---|---|---|---|---|---|
| LG OLED C3 | 9.2.1 (23.20.59) | Earlier development builds; see individual PRs for validation | Launcher and Control Center development checks reported in this repository | This is not an acceptance report for every feature or release; two-pane Settings validation is still pending | Home+'s author; development notes in `CLAUDE.md` |
| LG OLED C1 | 6.5.3 (03.53.54) | 0.21.0 (0.20.0 couldn't finish setup on webOS 6's Node 8) | Setup and elevation, the boot script, Continue Watching from Jellyfin | Without LG Input Hook: the Control Center over other apps and the Home button not tried; Stremio, Trakt, Plex and TMDB not reported | [mattokar](https://github.com/mattokar), [#37](https://github.com/pbalazaar/home-plus/issues/37) |
| LG OLED C4 (OLED65C4PSA) | 26, build 11.2.0-3703 (43.21.73) | 0.22.0, with [#43](https://github.com/pbalazaar/home-plus/pull/43)'s Trakt sign-in (without it, 0.22.0 couldn't sign in with a new Trakt Development app) | Setup, with the service still running after a TV restart; the Home button opening Home+ (LG's home screen shows for a moment first); Continue Watching from Stremio and Trakt (Trakt refreshed its token and stayed signed in after a restart) | No Flash isn't offered (LG's home screen is a Flutter app on webOS 26); without LG Input Hook, not tried: opening apps and HDMI inputs from Home+, the Control Center, standby and wake, giving the Home button back to LG | [MoBluey](https://github.com/MoBluey), [#44](https://github.com/pbalazaar/home-plus/issues/44) |

**How to read it:** "webOS" and "firmware" are as **Settings → General → About This TV** shows them. "Works" lists what the reporter tried. The Control Center over other apps and a Home button without a flash need [LG Input Hook](https://github.com/Simon34545/lginputhook), so a report says whether it was installed.
