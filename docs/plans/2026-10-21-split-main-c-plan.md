# Split the launcher's main.c

Carried out on 2026-10-21, steps 0 to 15, as section 9 describes, with the few differences it lists.

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
| 592–1164 | items and layout, `parse_apps`, the Top Shelf rail (`rail_fill`, `parse_continue`), `parse_control`, `parse_featured`, `parse_system`, plus 24 forward declarations | 570 |
| 1165–1442 | uniform tiles: `make_cover`, `make_logo` (pixel code; reads only `S.vivid_covers`) | 280 |
| 1443–1741 | `handle_result`: one switch over the 29 `TAG_*`s, some with long inline bodies (sign-in, Trakt, prefs) | 300 |
| 1742–2015 | launching, sheet open/close, focus rects, Recents (`sw_*`) | 270 |
| 2016–2165 | first run (`setup_*`, `on_status`, `on_elevate`) | 150 |
| 2166–2725 | Settings pages, swatches, and `run_action` (a 300-line switch with 43 cases over the 46 `ACT_*`s) | 560 |
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
   - render every preview screen into `build/screens-before/`, twice: the second run measures the noise (see section 4);
   - add `tools/native-preview/compare-screens.py` (pixel diff against the baseline, with the clock masked and a
     tolerance for the scenes caught mid-animation);
   - make `test/qml-sizes.test.js` read its `#define`s from `home.h`, and run its apostrophe and "No room" checks over
     every `native/src/*.c` instead of `main.c` alone. This isn't optional: tried on a copy after steps 1–3, three of
     its six tests fail (the `#define`s are gone from `main.c`), and the copy test keeps passing while it no longer
     sees the strings that moved.
1. **`home.h`**: lines 32–432 (layout, enums, types, `State`). `TINTS`/`NTINTS` (Settings, prefs, `draw_face`, the sheet,
   input) and `SRC_ID`/`SRC_NAME` (the shelf, sign-in, Settings, actions, drawing, `main`) are used from five or six of
   the new files, so they go in `home.h` as `static const`: GCC's `-Wall` doesn't warn about an unused `static const`
   from a header in C (that's `-Wunused-const-variable=2`, which nothing enables; checked). `CC_ICONS` (only `cc_icon`)
   and `LOOK_NAME` (only Settings) stay in their own file, for tidiness rather than warnings.
2. **`util.c`**, with `approach`, `spring`, `smooth01` and the one-line `tex_free` as `static inline` in `home.h` (the
   first three run for every element, every frame, so they should still inline; `tex_free` is called everywhere).
3. **`tiles.c`**: pixel code that reads `S.vivid_covers` and calls only `r_tex`, `tex_free` and `smooth01`. This is the
   easiest cut.
4. **`draw.c`, primitives only**: `draw_text`, `sway`, `glass`, `panel`, `ensure_label`, `ensure_big`, `lift`,
   `draw_face`, `draw_skeleton`, `draw_scrims`, `draw_marquee`. These come early because the feature files' drawing
   calls them.
5. **`sheet.c`**
6. **`recents.c`**
7. **`cc.c` and `cc_list.c`**. The file-scope `special_s`/`special_v` (the Hidden tile's focus) are used by `update()`
   (which stays in `main.c`) and `draw_tile` (`draw.c`), so they can't stay file-scope: they become two fields of
   `State`, the only renaming besides `S`'s type.
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
- `tools/test.sh screens`, then `compare-screens.py build/screens-before build/screens`. `check-screens.py` alone isn't
  enough: it only checks that each scene isn't blank and differs from the home screen by more than 0.1% of its pixels,
  so a scene can be wrong by less than that and pass.

  The screens aren't pixel-identical from run to run, so "identical" can't be the bar. Measured, two runs of the same
  binary: 33 of 36 scenes match outside the clock (at most a 16-level difference in a few pixels); `cc-editing`,
  `cc-options` and `move-hint` differ by up to 0.3% of their pixels, all inside the wobbling controls or the picked
  tile, and `crossfade` can catch a fade a step apart (later measured at up to 1.8% between two renders of b9d1d99,
  so it gets 3%). The clock capsule (x 1660–1802, y 50–110 on these runs) differs
  whenever the minute changes. So the script:
  - masks the band x 1580–1880, y 40–120;
  - counts the pixels that differ by more than 16 levels, and prints that share and its bounding box per scene;
  - passes a scene below 0.5%, and prints the rest for a look. A moved block that changed behaviour shows up as a
    difference elsewhere than those boxes, or on a scene that's otherwise still.

  A deterministic preview (a fixed step per frame when `HOMEPLUS_SCREENSHOT` is set) would make the bar "identical",
  but that's a code change, so it's in section 6.
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
  `minutes`, `activate`, `lift`, `sway`) become external symbols. Checked against the exports of libc, libm, SDL2 and
  cJSON: none clash. The binary doesn't export them dynamically (no `-rdynamic`), so they can't interpose on the
  libraries `luna.c` loads with `dlopen`. If the TV's linker still reports a duplicate, rename that one function in its
  own commit.
- **Inlining.** Calls across files at `-O2` no longer inline. The hot helpers become `static inline` in `home.h` (step 2).
  Everything else runs once per frame or per event, so the cost doesn't matter. `-flto` is available if a profile ever
  disagrees.
- **Preview-only paths.** Not a risk, as it turns out: `main.c` reads only three variables (`HOMEPLUS_SCREENSHOT`,
  `HOMEPLUS_SCREENSHOT_MS`, `HOMEPLUS_REST_REPORT_MS`), all inside `main()`, which doesn't move; the key and pointer
  scripts live in `preview.c`. Nothing in `tools/`, `docker/` or CI names a source file: the Makefile's `SRC` is the
  one list, and the TV build goes through the same Makefile.

## 6. Later, optional: changes that aren't only moves

Each of these is a separate PR after the split, and each still renders the same screens:

- **A deterministic preview.** With `HOMEPLUS_SCREENSHOT` set, step the animations by a fixed `dt` per frame (and show
  a fixed time on the clock), so two renders of one build are identical and `compare-screens.py` can demand that.
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

## 8. What was checked

Done on a copy of the tree on 2026-10-21, on a Linux desktop (the host build, Xvfb and Mesa), before this plan was
settled; the numbers above come from it:

- **Steps 1–3, as a dry run.** A script cut `home.h` (lines 32–432 with `typedef struct {...} State; extern State S;`,
  `CC_ICONS` and `LOOK_NAME` left in `main.c`, plus the four inline helpers and two prototypes) and `tiles.c` (lines
  1165–1442, `make_cover` and `make_logo` no longer `static`) out of `main.c`, and added `src/tiles.c` to `SRC`. It built
  with no warnings under `-Wall -Wextra -Werror` at the first attempt. `main.c` went from 5,400 to 4,734 lines.
- **The screens of that build**, all 36, against two renders of the unchanged build: the same outside the clock, except
  the three animated scenes above, which differ from each other by the same amounts the two baseline renders do.
- **`test/qml-sizes.test.js` on that copy**: 3 of 6 tests fail, and the copy test passes while covering less. Hence
  step 0.
- **The names that lose `static`**: looked up in `nm -D` of libc, libm and SDL2, and in cJSON's header: no clash.
- **`-Wunused-const-variable`**: a `static const` table in a header included by a file that doesn't use it gives no
  warning under `-Wall -Wextra` in C; it does under `-Wunused-const-variable=2`, which the Makefile doesn't set.
- **Not checked:** the TV build and the package check, which need the ARM toolchain, and the TV itself. Those are
  still the end-of-PR checks in section 4.

## 9. How it was done

The steps were made by a script, not by hand: it cuts the original `main.c` into its top-level pieces (each definition
with the comments before it), puts each in its file by a table of names, takes `static` off a function only when another
file calls it, and writes that function's prototype into `home.h`. A define, enum or table goes to the one file that uses
it, or to `home.h` when several do. Each step is that script run with one more file in the table, so each commit is the
one before plus that file's moves. It was run on `main.c` as of b9d1d99 (after its code review fixes, which added
`rail_clear`; that went to `shelf.c`).

Every step was checked before its commit:
- the host build and the TV build (the toolchain from `docker/toolchain.sha256`, GCC 16.2), both with no warnings under
  `-Werror`;
- every one of the original's 200 functions found exactly once, its body byte-for-byte the original's but for `static`
  (or `static inline`), `S.special_s`/`S.special_v` (step 14), and `handle_result`'s two sign-in cases (step 10);
- `test/qml-sizes.test.js`.

The screens were rendered once, for the end result (step 14), against the original's, rather than for each step: since
every step is generated from the original and its bodies were checked, a difference would be bisected to its step
afterwards.

Where it differs from sections 2 and 3:
- **The sign-in functions** are `on_media_signin()` and `on_trakt_signin()`, not `on_media_link()` and `on_trakt()`: each
  handles both the link and the status answer.
- **`keep_in_view`** went to `util.c` (`update()` and Settings both use it, to scroll a sheet's list), and **`parse_system`** to `cc.c` (it fills
  the Control Center's list of settings apps). **`update_clock`** went to `draw.c` with `draw_clock`.
- **`SRC_ID` and `SRC_NAME`** stayed in `main.c` until other files used them, then moved to `home.h` as planned.
- **Line counts** came out a little different from section 2's estimates: `main.c` 493, `home.h` 671, `cc.c` 652,
  `shelf.c` 532, `input.c` 455, `draw.c` 405; the rest between 102 and 310.
