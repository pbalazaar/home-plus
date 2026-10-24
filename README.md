<p align="center">
  <img src="docs/banner.jpg" alt="Home+ on an LG TV: what's popular on Prime Video in the Top Shelf, over the dock's apps as clear glass tiles" width="100%">
</p>

# Home+

[![Latest release](https://img.shields.io/github/v/release/pbalazaar/home-plus)](https://github.com/pbalazaar/home-plus/releases/latest)
[![CI](https://img.shields.io/github/actions/workflow/status/pbalazaar/home-plus/ci.yml?branch=main&label=CI)](https://github.com/pbalazaar/home-plus/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/github/license/pbalazaar/home-plus)](LICENSE)
[![Downloads](https://img.shields.io/github/downloads/pbalazaar/home-plus/total)](https://github.com/pbalazaar/home-plus/releases)
[![Platform: rooted LG webOS TVs](https://img.shields.io/badge/platform-rooted%20LG%20webOS%20TVs-blue)](#requirements)

**A home screen replacement for rooted LG webOS TVs.** Set as your home screen, it's there after the TV starts, the Home button goes to it, and closing an app comes back to it. Its Control Center handles the settings you change every day, and it shows no ads. tvOS-style, in a Liquid Glass look, with Continue Watching from Plex, Jellyfin and Stremio. Free and open source.

> **Can I use it?**
> - Your LG TV is **rooted** and has the [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel). Most current LG firmware can't be rooted; the Homebrew Channel's page explains how to check yours.
> - So far it's **tested only on an LG C3** with webOS 9.2.1 (firmware 23.20.59). Other models and webOS versions may work but haven't been tried; reports from other TVs go in [COMPATIBILITY.md](COMPATIBILITY.md).
> - Optional: [LG Input Hook](https://github.com/Simon34545/lginputhook), so LG's home screen never flashes on the Home button and the Settings button opens Home+'s Control Center over any app.

**Install:** download the `.ipk` from the [latest release](https://github.com/pbalazaar/home-plus/releases/latest), install it with [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) (Apps → Install), and open **Home+**. Changed your mind? **Settings → About → Remove Home+** removes its setup and uninstalls it. More in [Install](#install).

![Home+ on an LG C3: navigating apps, Recents, Control Center, and two-pane Settings, with Prime Video](docs/screenshots/homeplus-demo.gif)

*Recorded on an LG C3. [Watch the MP4](docs/screenshots/homeplus-demo.mp4).*

*Screenshots show a real TV's apps and Continue Watching. The Jellyfin one uses placeholder shows.*

> **Not affiliated with LG, Apple, Stremio, Trakt, Jellyfin, Plex or TMDB.** Home+ uses the TMDB API but is not endorsed or certified by TMDB. Home+ is an independent project. tvOS and Liquid Glass are Apple's names for Apple's designs; Home+ only borrows the look. It runs on **rooted** TVs and uses Stremio's, Trakt's and Plex's web APIs unofficially. Use it at your own risk.

## What it does

- **Top Shelf:** focus an app in the top row and the screen fills with its pictures, one title at a time, like tvOS. Stremio, Jellyfin and Plex show what you're partway through; streaming apps show what's popular on them (with a free TMDB key); other apps get a glow taken from their icon.
- **Liquid Glass:** the dock, clock, buttons and menus bend the light at their edges and blur only what sits behind text. Focused tiles lift, sway and catch a moving light. The Magic Remote pointer steers that light.
- **App tiles:** like iOS 26's, every app can show its own colours, sit on a dark tile, or turn to glass: clear, or tinted in one of ten colours.
- **Motion and sound:** everything moves on springs, and menus grow out of the button that opened them. A soft tick plays when focus moves; **Settings → General → Sounds** turns it off.
- **Continue Watching:** what you're partway through in Stremio (with your Trakt progress), Jellyfin or Plex, newest first. It turns to the next title every 10 seconds (or stays still: **Settings → Display & OLED → Top Shelf**); press **Up**, then **Left** and **Right**, to step through them. You sign in with a code or a QR code, so no password is typed on the TV.
- **Recents:** an App Switcher for the apps you opened that are still running, most recent first. Apps webOS runs by itself (preloaded, or started again in the background) stay out. Open them, close them one by one, or close them all. Press **Back** on the first app to open it.
- **Your layout:** hold **OK** on an app to move it, move it to the front, hide it, or delete it from the TV (like tvOS's Delete App; LG's built-in apps, the Homebrew Channel, LG Input Hook and Home+ itself can't be deleted). Hidden apps collect in a "Hidden" tile at the end.
- **Your home screen, all day:** optionally, the Home button opens Home+ instead of LG's home screen, closing an app comes back to it, and it's what you see after the TV starts.
- **No ads:** no sponsored rows or tiles. Home+ connects only to the services you sign in to (see [Privacy and security](#privacy-and-security)).
- **OLED care:** the picture drifts by a pixel every minute, and an idle Home+ dims, then goes black.
- **Light on the TV:** a native app (C, SDL2, OpenGL ES 2) that draws nothing while nothing moves. It shares the GPU happily with PicCap and HyperHDR.

## Requirements

- A **rooted** LG TV running **webOS 5 (2020) or later**, with the **[Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel)** installed. Whether yours can be rooted depends on its firmware: [CanI.RootMy.TV](https://cani.rootmy.tv/) says. webOS 4 and older can be rooted too, but their Node.js (0.12 or older) can't run Home+'s service.
- **Tested on:** LG C3 (OLED), webOS 9.2.1 (firmware 23.20.59). [COMPATIBILITY.md](COMPATIBILITY.md) lists reported TV checks. CI checks every build against LG's firmware for webOS 5 to 26 with [webOSbrew's compatibility checker](https://github.com/webosbrew/dev-toolbox-cli), and runs the service's tests on each one's Node; no TV but the C3 has tried it yet. If you try one, please [send a compatibility report](https://github.com/pbalazaar/home-plus/issues/new?template=compatibility.yml), whether it went well or not.
- **Optional:**
  - for Continue Watching, any of: a [Stremio](https://www.stremio.com) account and the Stremio app; a [Jellyfin](https://jellyfin.org) server and the Jellyfin app; a [Plex](https://www.plex.tv) server and the Plex app;
  - a [Trakt](https://trakt.tv) account;
  - [lginputhook](https://github.com/Simon34545/lginputhook), for a Home button without a flash (see [Home button](#home-button)).

## Install

1. Download `com.github.pbalazaar.homeplus_<version>_all.ipk` from [Releases](../../releases).
2. Install it with [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) (Apps → Install), or with `ares-install`.
3. Open **Home+** from the app list.

**What the first start does:** Home+ asks the Homebrew Channel for the root access its background service needs, restarts that service and links its boot script into the Homebrew Channel's startup scripts, so it's ready every time the TV starts. You'll see "Setting Up Home+" for a few seconds. Then Home+ asks whether the Home button should open it. You can change that answer any time in Settings.

If something stops it, the screen says what and offers **Try Again**:

| Screen | What it means |
|---|---|
| Homebrew Channel Needed | The Homebrew Channel isn't installed, or isn't running as root. |
| Couldn't Get Permission | The Homebrew Channel refused to give Home+ root access. Check that the TV is rooted and the Homebrew Channel is up to date. |
| Home+ Couldn't Start | Home+'s background service didn't answer. Restarting the TV usually fixes this. |

Updates install the same way. An update resets the service's root access, and Home+ sets it up again the next time it starts.

## Using Home+

| Key | What it does |
|---|---|
| Arrows, **OK** | Move and open. Hold **OK** for an app's options. |
| **Back** | Closes a menu, steps out of Continue Watching or the clock, then jumps to the first app. On the first app it opens Recents. It never leaves Home+. |
| **Red** | Recents |
| **Green** | Settings |
| **Yellow** | The focused app's options |
| **Blue** | Move the focused app (press again when it's in place) |

![Recents, an App Switcher like tvOS's](docs/screenshots/recents.jpg)

**Magic Remote pointer:** point at anything and click. Pressing an arrow hands control back to the keys: then OK presses what the keys focused, wherever the cursor happens to be. Moving the pointer clearly takes over again.

From the top row, press **Up** to reach the clock at the top right, as on tvOS; **OK** on it opens the Control Center. On Stremio, Jellyfin or Plex, Up goes into Continue Watching first, as the chevron above the dock shows. The remote's own **Settings** and **Input** keys open LG's menus; apps never receive them. With LG Input Hook installed (see [Home button](#home-button)), **Settings → Remote → Settings Button** makes the Settings key open the Control Center: over Home+, and over any other app or input too, where it opens on top of what's playing and the video carries on underneath (see below). **Input Button** does the same for the Input key, with the Control Center's inputs on their own: **OK** switches to one, and **Back** or the Input key again closes them.

![Settings](docs/screenshots/settings.jpg)

![Control Center](docs/screenshots/control-center.jpg)

**Control Center**, like tvOS's, opens from the clock: glass tiles under it for **Power Off**, **Wi-Fi** (your network; OK opens LG's network settings), **Screen Off** (sound keeps playing; any key brings the picture back), **Sound** (OK switches between TV speakers and HDMI ARC or eARC, and any other output the TV has used, like Optical: not a full picker), **Sleep Timer** (the minutes left; OK steps through LG's 10 to 240 minutes, and the TV turns off when it runs out), **Picture** (the picture mode; OK lists the modes LG offers for what's on, which differ for HDR and Dolby Vision), **Eye Comfort** (OK turns it on or off), and round buttons for **Inputs** (Live TV and the HDMI ports by LG's names, with what's plugged in, like "Soundbar · PS5"), **Recents**, **Accessibility** and **Settings**. A name too long for its tile scrolls while the tile has focus. Picture and Inputs open their list in the Control Center's place.

**Over other apps**, the Settings key opens the same Control Center on top of what's playing, as LG's Quick Settings would open, and the video keeps playing under it. Its glass, like tvOS's, takes its look from the picture behind it: the TV hands Home+ a small frame of the video about three times a second, blurred, so the glass carries the picture's colours rather than its detail, and the corner behind the clock dims only where the picture is bright (a channel's logo, say). The TV can't always capture its video: protected video (Netflix and the like) gives no frames, and nor does any video while PicCap captures it, as the TV captures video for one app at a time. The glass is then a clear tint, unless **Blur Over Video** (Settings → Control Center) pauses PicCap while the Control Center is open. Everything works there as in Home+, editing too, with Home+'s focus sounds and its Reduce Motion and Reduce Transparency settings; **Back** closes it, and so does the Settings key again. Recents, Accessibility and Settings open Home+.

![Add a Control](docs/screenshots/control-gallery.jpg)

**Make it yours**, like iOS 18's: hold **OK** on a control and choose **Edit Controls**. The controls wobble; **OK** picks one up and the arrows move it, holding **OK** changes its size (Small, Wide, Large) or removes it, and the **+** slot adds one: **Sound Mode**, **OLED Brightness**, **Energy Saving**, **Game Optimizer** and **Dynamic Tone Mapping** (whichever your TV has), and **Ambient Light** and **Screen Capture** when HyperHDR and PicCap are installed (they turn HyperHDR's LEDs and PicCap's capture on and off). There's room for seven rows: a size or a control that won't fit is turned down, and the Control Center says so. **Reset** puts it back, and **Back** is done; every change is saved as you make it. Each action is also a service method, so a script, a home automation or a key-mapping tool running on the TV can use it with `luna-send` (as root: Home+'s luna methods answer only `luna-send` and Home+ itself, not other apps):

```sh
luna-send -n 1 luna://com.github.pbalazaar.homeplus.service/control '{"action":"screenOff"}'
# actions: powerOff, screenOff, sound ("next" or an output), sleepTimer ("next", "off" or minutes),
#          pictureMode (a mode, like "cinema" or "filmMaker"), eyeComfort ("next", "on" or "off"), input (an input's app id),
#          run (a control's id, like "soundMode" or "ambientLight": it steps or switches), reset (the default controls)
```

**Settings** is two panes: its sections down the left, each with its icon, and the rows of the one in focus on the right, each with a line on what it does. **Up** and **Down** move through the sections, **OK** or **Right** goes into one, and **Left** or **Back** comes back out. A row with a value (On or Off, a time, a choice) shows it between **‹ ›**: **Left** and **Right** change it where it is. A line at the bottom says what the keys do.

| Section | What's there |
|---|---|
| **Remote** | Whether the Home button opens Home+ or LG's home screen, No Flash on TVs that support it, and with LG Input Hook, what the Settings and Input buttons open |
| **Display & OLED** | Dim After, Turn Off After, Pixel Shift, Calm Colours (dark app backgrounds, so ambient lights don't flood the room), and whether the Top Shelf turns pages by itself |
| **App Tiles** | Default, Dark, Clear or Tinted, and Tinted's colour |
| **Control Center** | With LG Input Hook and PicCap: Blur Over Video, which pauses PicCap's capture while the Control Center is open over other apps, so its glass can blur the video |
| **General** | Focus sounds, a 12-hour or 24-hour clock, and Hidden Apps (once you've hidden one) |
| **Accessibility** | Reduce Motion, Reduce Transparency |
| **Accounts** | Stremio, Trakt, Jellyfin, Plex and TMDB: whether they're connected, signing in and signing out |
| **TV Settings** | LG's own settings apps |
| **About** | Version, whether Home+ is set up, the last remote key it ignored, licence, and **Remove Home+** (undoes everything and uninstalls, see below) |

![App tiles: Default, Dark, Clear and Tinted](docs/screenshots/app-icons.jpg)

**App Tiles** (in Settings) changes every app's tile, like iOS 26's icon styles. **Default** keeps each icon's own colours. **Dark** puts the logo on a dark tile in its own colours: a light icon's black lettering turns light, and a white logo on a coloured icon takes that colour, as Prime Video's does. **Clear** turns the tiles to frosted glass with white logos, coloured by the picture behind them. **Tinted** is glass in a colour, with the logo in a pale tint of it.

![App Tiles in Settings, with Tinted's colours](docs/screenshots/app-icons-settings.jpg)

With Tinted on, ten colours appear under the four looks: **Left** and **Right** pick one, and the tiles behind the menu change as you go (with the pointer, click one).

## Continue Watching

Focus Stremio, Jellyfin or Plex in the top row, and the Top Shelf shows what you're partway through there: one title at a time, with its picture, its logo and how far you are, turning to the next every 10 seconds. Press **Up** (the chevron above the dock says you can), then **Left** and **Right**, to step through them.

![Continue Watching in the Top Shelf](docs/screenshots/continue-watching.jpg)

Sign in under **Settings → Accounts**, or hold **OK** on the app and choose **Sign In**. The TV shows a code, and a QR code where the service has one; you approve it on your phone or computer, where you're already signed in. No password is typed on the TV.

![Signing in to Plex with a QR code](docs/screenshots/plex-signin.jpg)

Pressing **OK** on a card opens the app. The LG apps can't be sent to a specific title from outside, so a notification names the title, ready to pick from the app's own Continue Watching.

### Stremio

1. Go to **Settings → Accounts → Stremio**.
2. Scan the QR code, or go to <https://link.stremio.com> and enter the code.

You can also sign in over SSH (Homebrew Channel → Settings → SSH Server):

```sh
luna-send -n 1 luna://com.github.pbalazaar.homeplus.service/stremioLogin '{"email":"you@example.com","password":"…"}'
```

The password goes only to `api.strem.io`. Either way, the TV keeps only the session key that Stremio returns.

### Trakt

Trakt's paused playback joins the same row. Stremio also reports to Trakt, so a title can appear in both; Home+ shows it once. Trakt doesn't record which app played something, so its titles open in Stremio.

Trakt needs an API app of your own (it's free):
1. Create one at <https://trakt.tv/oauth/applications/new>, with `urn:ietf:wg:oauth:2.0:oob` as the redirect URI.
2. On the TV, go to **Settings → Accounts → Trakt** (or the welcome steps' Continue Watching) and scan the QR code
   with your phone, on the same network as the TV. Paste the Client ID and Secret into the page it opens.
   Or, over SSH:
   ```sh
   luna-send -n 1 luna://com.github.pbalazaar.homeplus.service/traktSetup '{"clientId":"…","clientSecret":"…"}'
   ```
3. Go to **Settings → Accounts → Trakt** again, then enter the code it shows at <https://trakt.tv/activate>.

### Jellyfin

![Jellyfin's Continue Watching in the Top Shelf](preview/native-jellyfin.png)

Sign-in uses Jellyfin's Quick Connect, so the server needs it turned on (**Dashboard → General → Enable Quick Connect**).
1. Go to **Settings → Accounts → Jellyfin**. Home+ finds the server on your network and shows a six-digit code.
2. In a Jellyfin app where you're signed in, open **Settings → Quick Connect** and enter the code.

The titles are what you're partway through, then the next episode of each show you're watching. The Accounts row appears once the Jellyfin app is installed.

**A server Home+ can't find** (on another network, or behind a reverse proxy): give it the address over SSH, then sign in as above.

```sh
luna-send -n 1 luna://com.github.pbalazaar.homeplus.service/jellyfinServer '{"url":"https://jellyfin.example.com"}'
```

### Plex

1. Go to **Settings → Accounts → Plex**.
2. Scan the QR code, or go to <https://plex.tv/link> and enter the code.

Home+ then picks your server, one you own first, and reaches it on your network when it can. The titles are the server's Continue Watching (On Deck on older servers). The Accounts row appears once the Plex app is installed.

### Popular on streaming apps (TMDB)

![Popular on Netflix](docs/screenshots/popular.jpg)

Streaming apps (Netflix, Prime Video, Disney+, Apple TV, Max and others) show what's popular on them in their Top Shelf, like the Apple TV app's "What to Watch". The titles come from [TMDB](https://www.themoviedb.org), which needs a free key of your own:
1. Make a TMDB account, then go to **Settings → API** and request an API key (or copy the API Read Access Token).
2. On the TV, go to **Settings → Accounts → TMDB** (or the welcome steps' Continue Watching) and scan the QR code
   with your phone, on the same network as the TV. Paste the key into the page it opens. Or, over SSH:
   ```sh
   luna-send -n 1 luna://com.github.pbalazaar.homeplus.service/tmdbSetup '{"key":"…"}'
   ```

Home+ checks the key with TMDB before keeping it, and `{"key":""}` removes it. The catalogs are your TV's LG store country's; if you watch from elsewhere, give the two-letter country code with `{"region":"AL"}` (`""` goes back to the TV's). Apps with no picture of their own (YouTube, Smart IPTV and the like) rotate through what's trending on TMDB instead, under "Trending Now".

![Trending Now behind YouTube](docs/screenshots/trending.jpg) An app counts as a streaming service when its name is one TMDB lists for your TV's country (its LG store country). Its 10 most popular series and films are fetched every 12 hours. **OK** opens the app and names the title, ready to search for: LG's apps can't be sent to a title.

## Home button

With **Settings → Remote → Home Button** set to **Opens Home+**, Home+ watches for LG's home screen and opens itself instead. LG's home screen shows for a moment first.

Leaving an app (its own Exit, or Back on its first screen) comes back to Home+ too, without LG's home screen: Home+ opens as soon as the app has closed, before webOS would open LG's (it waits a second for that). Coming back, the home screen comes into focus where you left it, on the app you opened, as tvOS's does.

To remove that flash, use lginputhook (1.5.1 or later, from the Homebrew Channel). It catches the Home button before LG's home screen starts. In the LG Input Hook app, add a keybind for button **773**, choose **Launch App** and pick **Home+**. Or put this in `/home/root/.config/lginputhook/keybinds.json`:

```json
{"773": {"action": "launch", "id": "com.github.pbalazaar.homeplus"}}
```

Home+ turns that keybind into a command that opens Home+ only while it's installed, and LG's home screen otherwise, so the Home button keeps working if Home+ is ever removed without **Remove Home+**. The Settings button's keybind (see [Using Home+](#using-home)) falls back to LG's Quick Settings the same way.

On TVs where LG's home screen is a web app (not the C3), **Settings → Remote → Home Button** also offers **No Flash**. In that mode, Home+ replaces LG's home page in memory while it runs; nothing on disk changes.

## OLED protection

A home screen keeps bright, static things in place for hours, so Home+ adds to LG's own Pixel Refresher and Screen Shift:
- **Pixel orbit:** every minute the whole picture moves by one pixel along a slow path, never more than 4 px from centre.
- **Idle dimming:** after 2 minutes without input, everything but the focused tile dims.
- **Black screen:** after 10 minutes it fades to black and stops drawing. The first key press only wakes it.

Change the timings, or turn any of these off, in **Settings → Display & OLED**. Calm Colours keeps app backgrounds dark there too, which HyperHDR and other ambient lights pick up.

## Removing Home+

**Settings → About → Remove Home+** undoes everything Home+ set up, then uninstalls it:
- it turns off the Home button takeover;
- it deletes its boot script, the boot script's log, and its settings;
- it deletes its sign-ins and keys (Stremio, Trakt, Jellyfin, Plex, TMDB);
- it removes its lginputhook keybinds, the Home key's and the Settings key's (your others stay);
- it deletes the root access files the Homebrew Channel created.

**If Home+ won't open,** do the same by hand over SSH:

```sh
rm -f /var/lib/webosbrew/init.d/50-homeplus /var/lib/webosbrew/homeplus-boot.log
rm -rf /var/lib/homeplus
luna-send -n 1 luna://com.webos.appInstallService/dev/remove '{"id":"com.github.pbalazaar.homeplus"}'
```

If you added the lginputhook keybinds, delete them in the LG Input Hook app.

**If Home+ is removed another way** (from the Homebrew Channel, say), it can't tidy up after itself, but what it leaves behind does nothing. The boot script is a link into Home+'s package, so it goes with Home+, and the Homebrew Channel skips the link left behind. The Home and Settings keybinds check for Home+ first, and without it open LG's home screen and Quick Settings, as the buttons do without Home+. What stays, to delete by hand if you like: those keybinds (in the LG Input Hook app), the link and its log (the first line above), and your settings, sign-ins and pictures (`/var/lib/homeplus`). Keybinds you made yourself for other buttons that open Home+ do nothing once it's gone, so delete those too.

If the TV ever has trouble booting, the Homebrew Channel's failsafe skips boot scripts after a crash during start-up, so the TV comes back without Home+'s.

## Troubleshooting

- **Home+ is stuck on a setup screen:** press Try Again. If that doesn't help, these logs on the TV say why:
  - `/tmp/homeplus-service.log`: the service.
  - `homeplus-native.log`: the launcher's own log, inside its app sandbox. Find it with `find / -name homeplus-native.log`.
  - `/var/lib/webosbrew/homeplus-boot.log`: the boot script, at the last start-up.
- **The Home button still opens LG's home screen:** check **Settings → Remote → Home Button**. If you use lginputhook, restart the TV after changing its keybinds.
- **No Continue Watching:** sign in under **Settings → Accounts**, and make sure the app's tile is in the top row.
- **A streaming app shows a glow, not what's popular:** check **Settings → Accounts → TMDB**, and that the app's name is one TMDB lists for your TV's country.
- **"No Jellyfin server found":** the TV and the server must be on the same network, with the server's auto-discovery on (it is by default). Or give Home+ its address (see [Jellyfin](#jellyfin)).
- **Plex: "none of your servers answered":** the server must be running and reachable from the TV. If it's only reachable from outside, turn on Remote Access in Plex.

## Questions

**Will it harm my TV?** Home+ runs a background service with root access and sets up a boot-script link and optional LG Input Hook keybinds. Root access and boot changes carry risk; testing on one TV cannot guarantee safety on every model or firmware. The keybinds fall back to LG's buttons when Home+ is gone. If the TV has trouble starting, the Homebrew Channel's failsafe can skip boot scripts. [Removing Home+](#removing-home) explains cleanup, including what a plain uninstall leaves behind.

**Can I go back to LG's home screen?** Yes. **Settings → Remote → Home Button** gives the Home button back to LG, and **Settings → About → Remove Home+** removes its setup and uninstalls.

**Does it work without root?** No. Opening apps, the Home button and the Control Center all need the root access the Homebrew Channel gives.

**Does it remove LG's ads everywhere?** No, only from the home screen, since Home+ replaces it. LG's screensaver, Live TV and its own apps are unchanged.

**Why does LG's home screen flash when I press Home?** Without [LG Input Hook](#home-button), Home+ can only replace LG's home screen once it has appeared. With it, the Home button opens Home+ directly.

**Why do I need my own TMDB or Trakt key?** Home+ doesn't come with keys of its own, so each TV uses its own. Both are free, and you paste them on your phone from a QR code, with no typing on the TV. Plex, Jellyfin and Stremio need no key.

**Does it work on my TV?** Maybe: only an LG C3 on webOS 9.2.1 has been tried so far, so other models and versions are unknown. [COMPATIBILITY.md](COMPATIBILITY.md) has the list, and a report from your TV, good or bad, adds to it.

## Privacy and security

- **What Home+ connects to:** only the services you sign in to: Stremio's and Trakt's APIs, `plex.tv` and your Plex server, your Jellyfin server, and TMDB (`api.themoviedb.org`, `image.tmdb.org`) if you add a key. Stremio artwork comes from `images.metahub.space`; Jellyfin and Plex artwork comes from your server. To find a Jellyfin server, Home+ sends one broadcast on your local network (UDP port 7359), only when you start a Jellyfin sign-in.
- **Signing out:** Settings → Accounts signs out of Stremio and ends Home+'s session on your Jellyfin server. Plex lists Home+ under your account's **Authorized Devices**; remove it there to revoke its access.
- **What's stored:** sign-ins, your layout and preferences, in `/var/lib/homeplus/config.json`, and the Top Shelf's pictures, so they show at once the next time, in `/var/lib/homeplus/images` (up to 64 MB). Both are readable only by root, and **Remove Home+** deletes them.
- **The local API:** the service runs as root and answers only on `127.0.0.1`. Every request needs a token the service writes into Home+'s app folder, and requests from web pages and other hosts are refused.
- **The keys page:** while the TV shows its QR code (Settings → Accounts → TMDB or Trakt), the service also answers on your local network, on a port of its own, at a link with 128 random bits in it that only the TV shows. It takes TMDB's key and Trakt's Client ID and Secret, nothing else, and closes once they're saved, when you close the QR code, or after 10 minutes.
- **The Control Center over other apps:** its glass is made from frames of the video, which only exist while it's open. They're kept in memory (`/tmp/homeplus-cc`), readable only by root, and deleted when it closes. Nothing is saved or sent anywhere. LG's compositor runs it from Home+'s app folder.

## Building from source

The launcher is built with the webOS TV toolchain ([openlgtv/buildroot-nc4](https://github.com/openlgtv/buildroot-nc4)), which runs on Linux. The easiest route is Docker, which works on Linux and macOS, Intel or Apple Silicon. On Windows, use WSL 2 ([how](CONTRIBUTING.md#building)).

```sh
tools/docker-build.sh                  # -> dist/com.github.pbalazaar.homeplus_<version>_all.ipk
tools/install.sh <tv-ip>               # copy it to the TV over SSH, install, set up root, open it
tools/test.sh                          # run the tests (tools/test.sh all: every check CI runs)
```

Without Docker, unpack the toolchain, run its `relocate-sdk.sh`, then `WEBOS_SDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot tools/build-release.sh`. `tools/native-preview/run.sh` renders screenshots on a Linux desktop. It runs the real service against a stand-in for webOS, and the shots land in `preview/`.

Every push is built and tested by GitHub Actions. Pushing a `v*` tag does the same, and once everything has passed, publishes a release with the `.ipk` attached. To send a change, see [CONTRIBUTING.md](CONTRIBUTING.md); everyone here follows the [Code of Conduct](CODE_OF_CONDUCT.md). Report security problems privately, as [SECURITY.md](SECURITY.md) explains. What Home+ does for accessibility, and what it can't do yet, is in [ACCESSIBILITY.md](ACCESSIBILITY.md).

## How Home+ is built and tested

Home+ is designed by its author, who makes every decision about what it does and how it does it. Claude, an AI assistant, helps write its code and its documentation. Development checks have been run on the author's LG C3 (webOS 9.2.1, firmware 23.20.59); the individual pull requests record what was tried and what is still pending. The launcher is a native app, written in C. On every change CI runs the service's tests against the Node versions in webOS 22–26, drives the Control Center's QML, and builds and checks the package. The launcher's screens are rendered and checked locally.

## How it works

One package holds three things: the launcher (C, SDL2 and OpenGL ES 2), a Node.js service that runs as root, and a boot script. The launcher talks to the service over a local HTTP API. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the details: the pieces, the API and the folders.

`CLAUDE.md` holds development notes, including the webOS quirks found along the way. Who makes Home+, and how: [How Home+ is built and tested](#how-home-is-built-and-tested).

## License

Home+ is released under the [MIT License](LICENSE). The third-party code it includes keeps its own licence; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):
- the SDL 2.0.14 and Khronos/SGI/Mesa OpenGL headers;
- cJSON;
- stb;
- the Inter font.
