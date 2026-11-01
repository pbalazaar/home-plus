# Settings and Recents over what's playing

The Control Center over other apps (`native/qml/`) sends Recents, Accessibility and Settings to Home+:
the launcher comes to the front, and what was playing goes behind it (Stremio doesn't survive that for
long). Those menus now open in the overlay, and the video keeps playing under them.

The launcher can't do this itself. It's an ordinary app, so coming to the front takes the playing app's
place; only a system UI that LG's compositor runs can draw over another app. So the menus are drawn in QML.

## What the owner chose

Two panes, as in Home+ (not a column in the Control Center's place): the same 1600 × 860 panel, pages on
the left, the page on show on the right, with the same keys and hints. About a third of the picture stays
clear; the rest is behind the glass.

## Settings

| Page | Over the video |
|---|---|
| Remote, Display & OLED, App Tiles, Control Center, Accessibility | Everything changes in place |
| General | Sounds and Clock change in place; Hidden Apps opens Home+ |
| Accounts | Each sign-in's state; OK opens Home+ (codes and QR codes, needed once) |
| TV Settings | Opens LG's apps, as from Home+ |
| About | The text; Remove Home+ opens Home+ |

A row that leaves the video says so in the hint: "OK opens Home+". The Accessibility tile opens Settings on
that page. Back goes from a page to the list, as in Home+, and from the list to the controls.

## Recents

A strip of cards along the bottom, each the app's icon on its colour, with Home+'s keys: Left and Right
pick, OK switches, Up closes, Down reaches Close All. Two differences from Home+, both because an app is
playing: focus starts on the app before the one in front, so OK flips back, and Close All asks first,
since it closes what's playing too.

## How it's built

- `service/settings.js` describes the pages as data, as `control.js` does the controls. The panel only
  draws them, so what can't be tested on a TV stays small and what decides things runs under Node's tests.
- `POST /api/settings` answers with the pages; `{ "row", "dir" }` changes a row first. A row is applied by
  the service from the pages it has just worked out, so the panel can only ask for what's on offer. Prefs go
  where `/api/prefs` puts them, and the Remote page's rows through what `/api/home` does (`setHome`).
- `POST /api/overlay` gains `{"event": "recents"}` (the apps, each with its icon as a `file://` path: a QML
  `Image` can't send the token), and `{"event": "open", "target"}` takes a Settings page for rows that open
  Home+. Apps it may open are the ones it was offered: inputs, Recents and LG's settings apps.
- `native/qml/SettingsPanel.qml` and `RecentsStrip.qml` draw them; `ControlCenterView.qml` opens them as two
  more kinds of list, so everything that already stands aside for a list does for these too. They're compiled
  with the rest while the service preloads the panel.
- Reduce Motion, Reduce Transparency, Sounds and the Clock apply to the open panel at once, and Blur Over
  Video to its frames. The launcher re-reads prefs when it comes back; Calm Colours also needs its covers
  remade then.
- Home+'s own Settings doesn't change. The words now live in both C and JS, so a test compares them.
- The panel sends one change at a time: two requests at once needn't reach the service in the order they were
  sent, and each change is made from what the last one left.
- The pointer takes focus only after real travel, as in Home+: Qt sends hover to whatever appears under a pointer
  that's lying still, and a panel this big in the middle of the screen is often under it. Sheets do the same, so
  a pointer lying still can't move Close All's question off Cancel.

## Glass

The panel is about twice the glass of the largest thing the overlay has drawn. Glass.qml's shader works out
the rounded edge five times a pixel; inside the bevel nothing it works out changes the colour, so the
inside of a panel takes a short way through.

## What only the TV can show

- Whether the compositor can load other apps' icons from disk. If not, a card shows the app's name.
- How the panel reads over real video, with and without frames (PicCap holds the capture on the C3).
- The load with PicCap and HyperHDR running.
