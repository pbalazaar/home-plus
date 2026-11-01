# Top Shelf: OK plays a Jellyfin title

Approved by the owner in conversation on 2026-10-30:
- Jellyfin first; Plex cards keep today's toast for now;
- OK resumes, and holding OK offers Play From Beginning and Open Jellyfin;
- the title is started through the Jellyfin server, in the app people already have;
- the behaviour, the structure, the session rule and the failure handling below, with the two-TV gap accepted.

Tried on the owner's C3 on 2026-10-31, and the approach works: section 6 has the results. They changed how Home+ finds the TV's session (section 3) and how it confirms that a title started, and they removed the resend (section 2). The owner asked for the design to be updated with them, and accepted the renamed-TV gap in section 3. 

Built on 2026-11-01. What building it changed is in place below, and section 6 has what the C3 and the test server showed: the tile keeps its line (the owner's decision), the app opens without waiting on the server, and Home+ registers before it reads the list.

## Goal

OK on a Continue Watching card opens the app, and a toast says where to find the title, because LG's apps can't be sent to a title from outside. On tvOS the Top Shelf plays it. With this, OK on a Jellyfin card plays the title from where you stopped, in the Jellyfin app you already use.

**Not doing:**
- Plex (section 7) and Stremio: Stremio's LG app ignores launch parameters, and there's no server that can tell it what to play;
- a player of our own: it would be a second package, and Jellyfin's app already has the subtitles, audio and picture handling;
- a deep link added to Jellyfin's app: it would wait on the Jellyfin project's releases, and then on everyone updating;
- a setting to turn it off: when it can't work, you get today's toast.

## What was found

- **Jellyfin's LG app reads no launch parameters.** [jellyfin-webos](https://github.com/jellyfin/jellyfin-webos) (`org.jellyfin.webos`, v1.2.2) loads the server's jellyfin-web in an iframe. Its device name is "LG Smart TV"; its client name isn't fixed (below).
- **The server can tell a connected client to play.** `POST /Sessions/{sessionId}/Playing` with `playCommand=PlayNow`, `itemIds` and `startPositionTicks` answers 204. It's the command Jellyfin's own Play On sends: on the C3, Resume in the browser sent the same call, plus `MediaSourceId` and `SubtitleStreamIndex`.
- **jellyfin-web obeys it.** `onPlay()` in `src/scripts/serverNotifications.js` calls `playbackManager.play()` with the item and `StartPositionTicks`, and `capabilities()` in `src/utils/dashboard.js` reports `SupportsMediaControl: true` for every host.
- **The TV can appear under two client names.** Signed in on the app's first connection, the session with the socket is "Jellyfin for WebOS", but from the next start on, playback is reported on a second session, "Jellyfin Web", which can't be controlled and has another `DeviceId`. Signed in on a later launch, there's one session, "Jellyfin Web". `DeviceName` was "LG Smart TV" on all of them.
- **A closed app can't be mistaken for an open one.** `SupportsRemoteControl` is true only while the client's socket is open (`SessionInfo.cs`, `WebSocketController.cs`). An app hidden behind Home+ keeps its socket for 40 to 50 s, and a PlayNow sent in that time waits until the app is opened.
- **Only your own sessions can be controlled**, unless your Jellyfin user has `EnableRemoteControlOfOtherUsers` (`SessionManager.cs`).
- **The launcher has room for this.** A long OK does nothing in the carousel today (`main.c` handles it only for tiles), and a `RailItem` already has the title's `id`.

## 1. What you see

- **OK on a Jellyfin card:** Jellyfin opens and the title plays from where you stopped. A Next Up episode plays from its start. A system toast says "Resuming *title*" (or "Playing *title*") while it gets there.
- **Hold OK on a card:** a small sheet named after the title, with **Play From Beginning** (only for something you've started), **Open Jellyfin** (just opens the app) and Cancel, as every sheet has.
- **The line under the title:** while stepping through titles, "Left and right for more · OK plays · Hold OK for options". With focus on the tile it stays "OK opens Jellyfin": OK there opens the app, and Hold OK gives the tile's own sheet.
- **If Jellyfin can't be reached in time:** the app stays open and you get today's toast ("*title* is in Jellyfin's Continue Watching"). It's never worse than now.
- **If you leave Jellyfin before it's ready:** Home+ drops the request, so nothing starts playing behind your back.
- **OK on another title while one is starting:** the new one replaces it.
- **Plex and Stremio cards:** no change.

## 2. How it works

The launcher stays thin, and the service does the waiting. Nothing runs at rest: the polling exists only for the seconds after an OK.

```
OK on a card
 └─ launcher: POST /api/resume { app, source: "jellyfin", id, fromStart }
     └─ service: finds the title in its list as it stands, opens the app, posts the toast, answers at once
         └─ in the background (jellyfin.js):
            1. registers with the server (4 s to answer), then reads the list again, so the position is current
            2. asks the server for the TV's Jellyfin session, once a second, for up to 25 s
            3. POST /Sessions/<session>/Playing?playCommand=PlayNow&itemIds=<id>&startPositionTicks=<position>
            4. watches this TV's sessions for up to 15 s, until one has the title as its NowPlayingItem
            5. if none does: today's toast
```

On the C3, with the server on the same network, the session took 4.5 s from a cold start and playback about 2.5 s more, so 25 s and 15 s leave room for a slower server. PlayNow worked the first time in all 10 tries, so it's sent once.

**Launcher (`native/src`):**
- `apps.c`: `resume_title(app, id, from_start)` posts `/api/resume` and sets `S.away`, as `launch_app()` does. `open_card_sheet()` sits next to `open_app_sheet()`. If the service refuses the title (it has left the list since the card came), `results.c` does what OK does on any card.
- `input.c`: on Jellyfin's carousel, `activate()` calls `resume_title()`, and its own toast just says "Opening Jellyfin…". The sheet's Play From Beginning is a new action; Open Jellyfin is the existing `ACT_LAUNCH_ID`. The sheet keeps the title's `id`, since the carousel can turn behind it.
- `main.c`: a long OK in `ZONE_RAIL` on Jellyfin's carousel opens the sheet.
- `shelf.c`: `rail_fill()` builds the hint line above for Jellyfin's carousel.

**Service (`service/`):**
- `service.js`: `POST /api/resume` refuses a title that isn't in Jellyfin's list, then opens the app with the same code as `/api/launch` (shared in a helper, toast included) and replies with the launch's answer. It looks the title up with `known()`, in the list as it stands: a list over a minute old would otherwise be a network read before the app opens. The toast is "Resuming" when there's a position and it isn't From Beginning, otherwise "Playing". Today's toast takes that one's place if it hasn't shown yet.
- `service.js` owns the run. A new request replaces the old run. Once the app has been in front, any other app coming to the front ends the run for good, even if you come back before it's ready, and `stillWanted()` is false for a run that has ended. (Right after the launch Home+ is still in front, so leaving only counts once the app has been seen there.)
- `jellyfin.js`: `toItem()` adds `positionTicks`. `playOnTV(item, stillWanted)` does steps 1 to 4 and answers true or false; on false with the run still wanted, `service.js` posts today's toast. Only `jellyfin.js` talks to the server.
- Step 1 forces a refresh of the list, and `items()` already keeps the last list through an outage. If the title has left the list by then (you finished it elsewhere), Home+ stops and shows today's toast.

Other features don't change: Jellyfin's "next episode" carries on as usual, and Home+'s return after an app closes works the same.

## 3. Finding the TV's session

Jellyfin lists every client on the server, so Home+ has to pick this TV's. The rule: **the controllable session named "LG Smart TV" that reaches the server from the same address Home+ does.**

- Home+ first registers itself with `POST /Sessions/Capabilities`. Jellyfin records a session and its address when that endpoint runs (`RequestHelpers.GetSession`). No media control is offered, so nothing can be sent to it. It has to: right after sign-in the entry's address is null, and after a server restart there's no entry until it does, whatever else Home+ reads.
- Home+ then reads `GET /Sessions` once a second. That one list holds Home+'s own entry (its `DeviceId`), which shows how the server sees this TV, and the candidates: `DeviceName` is "LG Smart TV", and `SupportsRemoteControl` is true. `Client` can't be used: the TV's session may be "Jellyfin for WebOS" or "Jellyfin Web" (section 6).
- The candidate must have the same `RemoteEndPoint` as Home+'s entry. Both are normalised the same way by the server. Two empty addresses aren't a match.
- If two candidates share that address, Home+ picks neither and carries on polling, so it ends in today's toast. It never guesses.
- A title has started when any session with that name and that address, controllable or not, has it as `NowPlayingItem` (compared by `Id`). That's often not the session PlayNow was sent to, and the two don't share a `DeviceId`.

Comparing with Home+'s own address, not the TV's LAN IP, also works when the server sees something else: Docker's NAT, a reverse proxy, or a server elsewhere. Under OrbStack, every client showed the gateway, 192.168.215.1.

**Known gap, accepted:** with two LG TVs behind a proxy that hides their addresses, if the other TV's Jellyfin app is open and this one hasn't connected yet, the other TV looks like the only match and the title plays there.

**Known gap, accepted:** a TV renamed in Jellyfin's dashboard isn't found, because `DeviceName` takes the new name (tried on the test server). It gets today's toast.

## 4. What can go wrong

| What happens | What Home+ does |
|---|---|
| The app is on its sign-in or server page (no session) | After 25 s, today's toast |
| The app is signed in as another Jellyfin user | Works only if Home+'s account is an admin that may control other users (others don't see that session in `GET /Sessions`); otherwise today's toast |
| Server unreachable | Today's toast right away |
| The server takes the call and doesn't answer (it's asleep) | The app opens at once; today's toast after 4 s |
| Home+'s token is rejected | Signs out as it does now, then today's toast |
| Two matching sessions | After 25 s, today's toast |
| The TV was renamed in Jellyfin's dashboard | After 25 s, today's toast |
| PlayNow sent but nothing plays in 15 s | Today's toast |
| You left Jellyfin after it came to the front | Stops quietly |
| You leave Jellyfin just after PlayNow is sent | A hidden app holds the command, so the title may start the next time Jellyfin opens |
| OK on another title | The new run replaces this one |
| The launch itself fails | Answered like a failed `/api/launch`; nothing runs in the background |

Each step goes to the service log (`jellyfin: resume ...`), without tokens.

## 5. Tests

- `test/jellyfin.test.js`: the session rule (same address, other address, other clients, closed socket, two at one address, both of the TV's client names), PlayNow's parameters, From Beginning, waiting for the session, a title that starts on a different session from the one PlayNow went to, giving up, and stopping when `stillWanted()` turns false. The timings are options, so the tests run fast.
- A new `test/resume.test.js`: `/api/resume` through the real service (`startService`), with a mock Jellyfin: an unknown title refused, the app opened, both toasts.
- `tools/native-preview/harness.js`: mock `/Sessions`, `/Sessions/Capabilities` and `/Sessions/<id>/Playing` under `HOMEPLUS_PREVIEW_MEDIA`, and a preview scene for the sheet (`tools/test.sh screens`).

## 6. TV experiments

Run on the owner's C3 on 2026-10-31, with Jellyfin 12.4.0 in OrbStack on the Mac, the app's 1.2.2, and the test movie below (H.264, a keyframe about every 10 s). All 10 PlayNows played on the first send, each from the keyframe before the position: asked for 5:00, it started at 4:51.

| Experiment | Result | What it changed |
|---|---|---|
| 1: Play On | Resume in the browser played from about 10:00 | The approach stands |
| 2: how the server sees the app | `Client` "Jellyfin for WebOS", `DeviceName` "LG Smart TV", `SupportsRemoteControl` true, `RemoteEndPoint` OrbStack's gateway | See the two rows on client names |
| 3: PlayNow | 204, and playing in about 2 s | The approach stands |
| 4: cold start, opened from Home+ | Session after 4.4 to 4.5 s, playing 2.3 to 2.7 s after PlayNow, 3 of 3 | The resend goes; 25 s and 15 s stay, for slower servers |
| 5: the app in the background | Still running, but its session stops being controllable after 40 to 50 s. A PlayNow at 15 s didn't play behind Home+: it played about 1 s after the app was opened. Opened after the session had gone, the session was back in 1.4 to 1.8 s | Opening the app before sending is enough |
| Added: OK on a card, app in the background | After 20 s: playing in 1.5 s. After 90 s: session in 1.8 s, playing 0.6 s later | Nothing extra to do |
| Added: playback after a cold start | Reported on a second session, "Jellyfin Web", with another `DeviceId`; the controllable session's `NowPlayingItem` stayed empty | A start is looked for on every session with the TV's name and address. Watching the controllable session would have seen nothing, resent at 15 s, then shown today's toast over a title that was playing |
| Added: signing in again on a later launch | One session, "Jellyfin Web": `Client` containing "webos" matched nothing. PlayNow to it played | The rule uses `DeviceName`, not `Client` |
| Added: a device renamed, on the test server | `DeviceName` took the new name, `HasCustomDeviceName` turned true | A known gap in section 3 |

Then on 2026-11-01, with Home+ itself, the test server still under OrbStack:

| Tried | Result | What it changed |
|---|---|---|
| Home+'s registration, with a real token, on the test server | 204, and its entry appears with its address. Right after sign-in the entry's address is null; after a restart a plain read makes no entry | An empty address is never a match |
| The same-address rule, with `jellyfin.js` against the server and a stand-in "LG Smart TV" on a real socket | From a container at another address (192.168.215.4): no PlayNow. From Home+'s own address: the socket got `Play` with `StartPositionTicks` as sent, 0 for From Beginning | The rule stands |
| OK on a card, on the C3, app closed | Session at 5.3 s, playing at 9.7 s, from 8:56, where it stopped | The approach stands |
| OK, the app in the background with the title paused | PlayNow at 0.2 s, playing at 1.3 s, from 9:16: the fresh position, not the list's 8:56 | Step 1 earns its read |
| OK on Next Up, and Play From Beginning | Each from 0:00, with "Playing", replacing what was paused | |
| Hold OK on both kinds of card, and Open Jellyfin | Both sheets as in section 1. Open Jellyfin: the app opens, nothing plays, no PlayNow | |
| Server refusing connections, on the C3 | Given up at 0.0 s, Jellyfin stays open, today's toast in the place of "Resuming" | |
| A server that takes the call and doesn't answer | Found by reading the code after the Mac slept with the test server on it: OK would have waited 10 s to open the app, then 20 s for the toast. In the tests now; not yet on the C3 | `known()`, and registering first |

Not covered: leaving Jellyfin before it's ready and that sleeping server on the C3 (both in the tests), a server on another machine, titles that need transcoding, and more than one TV.

To run them again: about 20 minutes, no ssh, with the Mac and the TV on the same network.

### Setup

A throwaway Jellyfin in Docker on the Mac. The test "movie" is a test pattern with a running clock, so the position can be read off the screen.

```bash
mkdir -p ~/jellyfin-test/media/Movies
```

```bash
docker run -d --name jellyfin-test -p 8096:8096 -v jellyfin-test-config:/config -v jellyfin-test-cache:/cache -v ~/jellyfin-test/media:/media jellyfin/jellyfin
```

```bash
docker exec jellyfin-test /usr/lib/jellyfin-ffmpeg/ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=24 -t 1800 -c:v libx264 -preset veryfast -pix_fmt yuv420p "/media/Movies/Test Clock (2026).mkv"
```

1. At <http://localhost:8096>, finish the setup wizard and add a Movies library on `/media/Movies`. Check that **Dashboard → General → Enable Quick Connect** is on, and make a key under **Dashboard → API Keys**.
2. Play the movie in the browser, jump to about 10 minutes, and stop. Keep this page open: the item's id is in the address bar (`details?id=…`).
3. On the TV, install Jellyfin from LG's store or the Homebrew Channel. (The store version needs any Homebrew or developer copy removed first.) Give it `http://<the Mac's IP>:8096` and sign in as the same user.

Then, in a Terminal on the Mac:

```bash
JF=http://localhost:8096
```

```bash
AUTH='Authorization: MediaBrowser Token="<the API key>"'
```

### Experiment 1: Play On

No commands. With the app open on the TV, click the cast icon at the top right of the movie's page in the browser, pick **LG Smart TV**, and press Resume.

Expected: the TV starts at about 10:00 on the pattern's clock.

### Experiment 2: how the server sees the app

```bash
curl -s -H "$AUTH" "$JF/Sessions" | jq '.[] | {Id, Client, DeviceName, RemoteEndPoint, SupportsRemoteControl, Playing: .NowPlayingItem.Name}'
```

Expected: an entry with `DeviceName` "LG Smart TV" and `SupportsRemoteControl` true (its `Client` depends on when the app signed in). Keep its `Id`. Under Docker, `RemoteEndPoint` is the gateway, not the TV's IP: that's why the rule in section 3 compares with Home+'s own address.

### Experiment 3: PlayNow

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X POST -H "$AUTH" "$JF/Sessions/<session Id>/Playing?playCommand=PlayNow&itemIds=<item id>&startPositionTicks=3000000000"
```

Expected: `204`, and the TV plays from 5:00 (3,000,000,000 ticks).

### Experiment 4: cold start

Close Jellyfin on the TV. Start this, then open Jellyfin with the remote:

```bash
while :; do date +%T; curl -s -H "$AUTH" "$JF/Sessions" | jq -c '.[] | select(.SupportsRemoteControl) | select(.DeviceName == "LG Smart TV") | {Id, LastActivityDate}'; sleep 1; done
```

Note how many seconds pass until a session line appears, then stop it with Ctrl-C. Close the app and repeat, this time sending experiment 3's PlayNow with the new `Id` as soon as the line appears.

Expected: the session appears well inside 25 s, and the title plays on the first PlayNow.

### Experiment 5: the app in the background

With Jellyfin open on its home page, go back to Home+ and wait a minute. Run experiment 2's command, and if the session is still there, send experiment 3's PlayNow. Listen for sound, then open Jellyfin.

### Afterwards

Keep the server for testing the feature itself. Home+'s network discovery won't get through Docker Desktop, so give Home+ the server's address as the README's Jellyfin section says (`jellyfinServer`). To clean up afterwards:

```bash
docker rm -f jellyfin-test
```

```bash
docker volume rm jellyfin-test-config jellyfin-test-cache
```

```bash
rm -rf ~/jellyfin-test
```

## 7. Plex, later

Plex cards keep today's toast. What's known, for a later version:
- The app is `cdp-30`. The owner's C3 doesn't have it, so a Plex version can't be tested there as things stand.
- Plex's remote control (Companion) did start titles on Plex for LG: [Home Assistant users got `play_media` working in 2020](https://community.home-assistant.io/t/media-player-play-media-to-plex-on-lg-webostv-does-not-work/181243?page=2) on version 4.29.2, and [another thread](https://community.home-assistant.io/t/possible-to-have-ha-open-plex-on-tv-and-then-play-a-tv-show/492205) launches `cdp-30` first, then sends `play_media`. Nothing confirms it on today's app (5.9x in early 2026), and [other reports say casting to it fails](https://forums.plex.tv/t/cannot-cast-anything-to-the-lg-plex-app/740296).
- The first thing to try: with the Plex app open, does `GET <server>:32400/clients` list it? In 2020 it did.
- No LG deep link for Plex is documented. [One forum thread](https://forums.plex.tv/t/deep-links-on-lg-smart-tvs/853082) reports deep links as broken on a new LG set.

A Plex version would add a `playOnTV` to `plex.js` behind the same `/api/resume`, and the launcher would treat Plex's carousel like Jellyfin's. If Companion doesn't work on today's app, the remaining option is a player of our own, as in [Luma](https://github.com/funnymataleao/luma-webos-launcher) (`extras/player`): HTML5 video, with progress through Plex's `/:/timeline`. That means a second package, and the [webosbrew listing](https://github.com/webosbrew/apps-repo/pull/289) would need a second entry, so it's the owner's call.

## 8. Docs to update

- `README.md`: "Pressing **OK** on a card opens the app…" in Continue Watching, the Jellyfin section (the app must be signed in as the same user), and the keys table if Hold OK needs a line.
- `CLAUDE.md`: a Known TV quirks entry for Jellyfin's app: no launch parameters, remote play through the server, and its two client names.
- `docs/ARCHITECTURE.md`: `POST /api/resume` in the API table.
