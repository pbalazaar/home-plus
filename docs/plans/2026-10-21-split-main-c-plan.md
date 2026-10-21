# Split the launcher's main.c

A proposal: nothing here is approved yet.

## Goal

`native/src/main.c` is 5,400 lines, and nearly all of the launcher lives in it: layout constants, every type, one
200-line global `S`, the service's answers, the Top Shelf, the Control Center, sheets and Settings, Recents, first run,
input, animation, drawing, the OLED idle stages and `main()`. The other native files (`render.c`, `gfx.c`, `net.c`,
`sound.c`, `luna.c`, `preview.c`) are already small libraries with their own headers, and they stay as they are.

The goal is to split `main.c` into feature files of about 150 to 600 lines, each holding one feature's data, logic and
drawing, with `main.c` left as start-up, the frame loop and `update()`.

**Not doing:** this changes no behaviour, no pixels, no names and no logic. Each step only moves code. Clean-ups that
change code are listed separately at the end (section 6) as later, optional work.

## 1. How the code is laid out today

The code already has section dividers (`/* ---- ... */`). Those sections are where the cuts go:

| Lines | Section | Size |
|---|---|---|
| 1–432 | layout `#define`s, enums, tables (`CC_ICONS`, `TINTS`, `LOOK_NAME`), types (`App`, `RailItem`, `CCItem`, `SheetItem`, `Pic`, `Action`), the global `S` | 430 |
| 433–591 | helpers: `approach`, `spring`, `toast`, `log_remote`, JSON and string helpers, `item_app`, `media_source` | 160 |
| 592–1164 | items and layout, `parse_apps`, the Top Shelf rail (`rail_fill`, `parse_continue`), `parse_control`, `parse_featured`, `parse_system`, plus 35 forward declarations | 570 |
| 1165–1442 | uniform tiles: `make_cover`, `make_logo` (pixel code only) | 280 |
| 1443–1741 | `handle_result`: one switch over 30 `TAG_*`s, some with long inline bodies (sign-in, Trakt, prefs) | 300 |
| 1742–2015 | launching, sheet open/close, focus rects, Recents (`sw_*`) | 270 |
| 2016–2165 | first run (`setup_*`, `on_status`, `on_elevate`) | 150 |
| 2166–2725 | Settings pages, swatches, and `run_action` (a 300-line switch over 43 `ACT_*`s) | 560 |
| 2726–3332 | Control Center: packing, editing, lists, gallery, `activate`, `move_focus`, `go_home` | 600 |
| 3333–3660 | input: remote keys, the pointer, `on_key_down`/`up`, hit-testing | 330 |
| 3661–3936 | animation: Top Shelf paging, `update()` | 275 |
| 3937–4913 | drawing: primitives, tiles, scene, sheet, Recents, Control Center, toast, `render()` | 980 |
| 4914–5159 | `load_assets`, idle wait (Wayland fd), OLED stages, going offscreen and coming back, Settings and Input keys | 250 |
| 5160–5400 | `main()` | 240 |

The forward declarations at lines 1131–1163 show how tangled the code is: those are the functions that sections call
before they're defined. Once the code is split, they become the shared prototypes.

## 2. Target layout

| File | What it holds (from today's lines) | Size, about |
|---|---|---|
| `home.h` | layout `#define`s, enums, types, `typedef struct {...} State; extern State S;`, `static inline` `approach`/`spring`/`smooth01`, and the prototypes each file shares, grouped by file | 550 |
| `main.c` | `main()`, `load_assets`, `request_data`, `open_asked`, `update()` | 600 |
| `util.c` | helpers (433–591): `toast`, `log_remote`, JSON and string helpers, `item_app`, `app_color` | 140 |
| `tiles.c` | `make_cover`, `spread`, `make_logo` (1165–1442) | 280 |
| `apps.c` | items and layout (592–753), `app_by_id`, `launch_app`, `close_apps`, `move_item_to`, the app and Hidden sheets | 320 |
| `shelf.c` | the Top Shelf: `Pic` helpers, rail texts, requests, `rail_fill`, `parse_continue`, `parse_featured`, paging (3684–3762), and its drawing (4126–4258) | 560 |
| `cc.c` | Control Center: `CC_ICONS`, `parse_control`, packing, editing, `cc_activate`, Settings and Input keys, `draw_control_center` | 620 |
| `cc_list.c` | its lists (Picture, Inputs) and the gallery: 2939–3098, `draw_cc_list`, `draw_cc_gallery` | 260 |
| `sheet.c` | the sheet engine: open, add, body, close, dismiss, layout (4410–4441), `draw_sheet`, the closing ghost | 320 |
| `settings.c` | `settings_row`, `settings_page`, swatches, `TINTS`/`LOOK_NAME`, `open_settings_sheet` | 270 |
| `actions.c` | `run_action` | 310 |
| `recents.c` | `sw_*`, `sw_key`, `draw_switcher`, `draw_switcher_card` | 230 |
| `setup.c` | first run (2016–2165), `setup_screen`, the uninstall answer | 190 |
| `accounts.c` | sign-in for Stremio, Jellyfin, Plex and Trakt: the bodies of those `handle_result` cases, as `on_media_link(r)` and `on_trakt(r)`; `source_installed` | 130 |
| `results.c` | `handle_result`, now only a dispatch | 150 |
| `input.c` | 3333–3660, `activate`, `move_focus`, `go_home`, `focus_rect`, `enter_top`, `dock_tile`, `to_logical` | 480 |
| `draw.c` | drawing primitives (`draw_text`, `sway`, `glass`, `panel`, `lift`, `draw_face`, skeletons, scrims, marquee), tiles, clock, scene, toast, `render()` | 520 |
| `idle.c` | idle wait, OLED stages, `next_wake_ms`, `release_images`, `go_offscreen`, `come_back`, `wake` | 200 |

**Choices this makes:**
- **Files by feature, not by layer.** A feature's drawing goes with its logic, as with Recents (`draw_switcher` needs `sw_*`'s
  state and layout) and the Control Center (`draw_control_center` needs `cc_rect`). `draw.c` keeps only what everything
  shares, and the home scene.
- **One internal header, `home.h`.** Every file reads and writes `S` and uses the layout, so 15 small headers would add
  noise without adding a boundary. Prototypes are grouped in `home.h` under a comment for each file. The existing
  libraries keep their own headers.
- **`S` stays `S`.** It becomes `typedef struct {...} State;` in `home.h`, `extern State S;`, defined once in `main.c`. That
  is the only renaming.
- **Functions stay `static` unless another file calls them.** Removing `static` only where it's needed keeps the shared
  surface honest: that list is what `home.h` declares.

## 3. Steps

Each step is one commit that only moves code: cut and paste verbatim, remove `static` where another file needs the
function, add its prototype to `home.h`, and add the file to `SRC` in `native/Makefile`. Steps go leaves first (code
that calls nothing else in `main.c`) so each one needs few new prototypes.

0. **Baseline and safety net** (before any move; see section 4):
   - render every preview screen into `build/screens-before/`;
   - add `tools/native-preview/compare-screens.py` (pixel diff against the baseline, ignoring the clock's rect);
   - make `test/qml-sizes.test.js` read its `#define`s from `home.h`, and run its apostrophe and "No room" checks over
     every `native/src/*.c` instead of `main.c` alone. Otherwise the checks would quietly stop covering moved code.
1. **`home.h`**: lines 32–432 (layout, enums, types, `State`). Tables that only one file uses (`CC_ICONS`, `TINTS`,
   `LOOK_NAME`, `SRC_ID`/`SRC_NAME`) stay as `static const` in their file. In a header they'd trip
   `-Wunused-const-variable` in every file that doesn't use them.
2. **`util.c`**, with `approach`, `spring` and `smooth01` as `static inline` in `home.h` (they run for every element,
   every frame, so they should still inline).
3. **`tiles.c`**: pure pixel code that only touches `App`. This is the easiest cut.
4. **`draw.c`, primitives only**: `draw_text`, `sway`, `glass`, `panel`, `ensure_label`, `ensure_big`, `lift`,
   `draw_face`, `draw_skeleton`, `draw_scrims`, `draw_marquee`. These come early because the feature files' drawing
   calls them.
5. **`sheet.c`**
6. **`recents.c`**
7. **`cc.c` and `cc_list.c`**. The file-scope `special_s`/`special_v` (the Hidden tile's focus) stay with tile drawing in
   `draw.c`.
8. **`shelf.c`**
9. **`apps.c`**
10. **`setup.c` and `accounts.c`**. Moving the sign-in case bodies out of `handle_result` into named functions is the one
    place where a step does more than cut and paste. It's still a verbatim move, wrapped in a function.
11. **`settings.c` and `actions.c`**
12. **`input.c`**. `ptr_lx`/`ptr_ly` stay `static` there.
13. **`idle.c`**. `wake_pipe` stays `static` there.
14. **`results.c`**, and the rest of the drawing (scene, tiles, clock, toast, `render`) into `draw.c`. What's left in
    `main.c` is section 2's list.
15. **Docs**: update the places that name `main.c` for something that moved:
    - `CLAUDE.md` ("OLED idle stages and pixel orbit in `main.c`", the input mode, `draw_face`, `make_logo`);
    - the comments in `tools/native-preview/glyphs.c` (`CC_ICONS`) and `glass-test.c` (the spacing system);
    - the first line of `main.c`, which describes the whole program and would then describe a file of start-up code.

    Leave the dated design docs as they are, since they record what was true then.

Steps 1–4 are worth landing as one PR first. They're quick, and they settle the header's shape. Steps 5–14 can go in two
or three PRs.

## 4. Checks for every step

- `make -C native` builds with no warnings, under `-Wall -Wextra` and a local `-Werror`. A function left behind that
  nothing calls now shows up as `-Wunused-function`, which is a useful signal for this refactor.
- `tools/test.sh screens`, then `compare-screens.py build/screens-before build/screens`: every scene must be identical
  outside the clock. `check-screens.py` alone isn't enough: it only checks that each scene isn't blank and isn't the
  home screen.
- `git diff --color-moved=dimmed-zebra --color-moved-ws=allow-indentation-change` shows that the moved blocks didn't
  change.
- `node --test test/*.test.js`, for the updated `qml-sizes` test.
- At least at the end of each PR: the TV build (`tools/docker-build.sh`) and `tools/test.sh package`, which checks the
  32-bit ARM binary still uses only SDL 2.0.14 functions. Then install it on the C3 and go through the home screen, Top
  Shelf, Control Center (with editing), Settings, Recents, and the idle dim and turning black.

## 5. Risks

- **Conflicts with work in progress.** Commits that only move code conflict with every open branch that touches
  `main.c`. Do the split when nothing else is open, keep each PR short-lived, and rebase other branches straight after.
- **Global names.** Functions that lose `static` (`toast`, `launch`, `wake`, `update`, `render`, `glass`, `panel`,
  `minutes`) become external symbols. They don't clash with libc, SDL or cJSON, and the binary doesn't export them
  dynamically (no `-rdynamic`), so they can't interpose on the libraries `luna.c` loads with `dlopen`. If the linker
  reports a duplicate, rename that one function in its own commit.
- **Inlining.** Calls across files at `-O2` no longer inline. The hot helpers become `static inline` in `home.h` (step 2).
  Everything else runs once per frame or per event, so the cost doesn't matter. `-flto` is available if a profile ever
  disagrees.
- **Preview-only paths.** `getenv("HOMEPLUS_…")` switches are spread through the code. Moving them doesn't change them,
  and the screens step runs every scene they drive.

## 6. Later, optional: changes that aren't only moves

Each of these is a separate PR after the split, and each still renders the same screens:

- **Split `update()` into per-feature `*_update(dt, &anim)` functions**, as `sw_update` already does for Recents
  (`shelf_update`, `cc_update`, `sheet_update`, `idle_update`). That brings `main.c` down to about 400 lines.
- **Group `S`'s loose fields** the way `S.recents`, `S.sheet` and `S.cc` already are: `S.shelf` (rail and paging),
  `S.prefs` (Settings' switches), `S.idle` (OLED), `S.ptr` (the pointer and which input is in charge). This renames a
  lot, so it needs its own PR.
- **Let each feature handle its own `run_action` and `handle_result` cases**, for example `ACT_CC_*` → `cc_action()` and
  sign-in tags → `accounts.c`. That removes the two long switches that know about every feature.

## 7. The other large files

These have the same problem, at a smaller size. They're worth splitting the same way after `main.c`, but they aren't
part of this plan:

- **`service/service.js` (1,614 lines).** Its section dividers already mark the cuts, and it already has a pattern for
  modules: `require('./x')({ readConfig, writeConfig, log, ... })`, as in `trakt.js`, `tmdb.js` and `control.js`. Likely
  modules:
  - `home.js`: Home button, no-flash, boot script;
  - `binds.js`: LG Input Hook's binds, the Settings and Input keys;
  - `overlay.js`: the Control Center over other apps, and its frames;
  - `images.js`: the icon and art proxy, and its cache;
  - `api.js`: `handleApi`;
  - `stremio.js`: Stremio's sign-in and Continue Watching.

  The service's tests already exercise these modules through the HTTP API.
- **`native/qml/ControlCenterView.qml` (1,368 lines).** It could split into components for the grid, the lists, the
  gallery and the sheets. `test/qml-sizes.test.js` and `tools/qml-check` cover it.
