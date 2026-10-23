#!/bin/sh
# Renders native Home+ screenshots into preview/native-*.png using the harness.
# HOMEPLUS_PREVIEW_OUT: another folder for them (tools/test.sh uses a fresh one).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${HOMEPLUS_PREVIEW_OUT:-$ROOT/preview}
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
ASSETS=${TMPDIR:-/tmp}/homeplus-native-preview
# the service writes its API token here and the launcher reads it (on the TV: the app folder)
export HOMEPLUS_TOKEN_FILE=${TMPDIR:-/tmp}/homeplus-api-token
# ...and its config goes here, so no root is needed
export HOMEPLUS_CONFIG_DIR=${TMPDIR:-/tmp}/homeplus-preview-config
python3 "$HERE/make-assets.py" "$ASSETS" "$ROOT/native/assets/fonts/Inter-Bold.ttf" > "$ASSETS.apps" && mv "$ASSETS.apps" "$ASSETS/apps.txt"
make -s -C "$ROOT/native"
service() { # start the harness service with the given environment
  [ -n "${SVC:-}" ] && kill $SVC 2>/dev/null && sleep 0.5
  env "$@" NODE_PATH="$HERE/stub" node "$HERE/harness.js" "$ASSETS" "$ROOT/service" & SVC=$!
  sleep 1
}
trap 'kill $SVC 2>/dev/null; rm -rf "$HOMEPLUS_CONFIG_DIR"' EXIT
service HOMEPLUS_PREVIEW=1
# Prefer a real GLES2 renderer (Mesa under Xvfb) so the glass effects show, like on the TV.
if command -v xvfb-run >/dev/null; then
  RUN="xvfb-run -a -s '-screen 0 1920x1080x24'"; DRIVER="SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=opengles2"
else
  RUN=""; DRIVER="SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software"
fi
shot() { # name keys ms [pointer points]
  LOG=${TMPDIR:-/tmp}/homeplus-preview-$1.log # the launcher's own output, shown if it fails
  if ! eval "$RUN env $DRIVER HOMEPLUS_SCREENSHOT='$OUT/native-$1.png' HOMEPLUS_KEYS='$2' HOMEPLUS_SCREENSHOT_MS='$3' HOMEPLUS_POINTS='${4:-}' '$ROOT/native/build/host/homeplus'" >"$LOG" 2>&1; then
    tail -20 "$LOG" >&2; echo "native-$1: the launcher failed" >&2; exit 1
  fi
  echo "saved $OUT/native-$1.png"
}
rest() { # name keys: run as on the TV for 32 s; in the last 10, the loop sleeps (a carousel turns once or twice): it
         # wakes 40 times a second at most (not a busy loop), and draws three quarters of the time at most: a loop that
         # never stops drawing draws all of it, however slowly this machine draws (a turn, on one slow CPU, under half)
  LOG=${TMPDIR:-/tmp}/homeplus-preview-rest-$1.log
  eval "$RUN env $DRIVER HOMEPLUS_REST_REPORT_MS=32000 HOMEPLUS_KEYS='$2' '$ROOT/native/build/host/homeplus'" >"$LOG" 2>&1 || true
  r=$(grep '^at rest:' "$LOG" || echo 'no report')
  wakes=$(echo "$r" | sed -n 's/^at rest: \([0-9.]*\) wakes.*/\1/p')
  drawing=$(echo "$r" | sed -n 's/.* drawing \([0-9]*\)% .*/\1/p')
  if [ -z "$wakes" ] || [ -z "$drawing" ] || awk -v w="$wakes" -v d="$drawing" 'BEGIN { exit !(w > 40 || d > 75) }'; then
    tail -5 "$LOG" >&2; echo "rest-$1: $r" >&2; exit 1
  fi
  echo "rest-$1: $r"
}
shot home "" 2500
shot stremio "RR" 3000
shot stremio-shelf "RRUR" 3500
shot scrolled "DR" 3000
shot sheet "H" 3500
shot topbar "U" 3000
shot control-center "UO" 3600
shot cc-picture "UODDLDO" 4400
shot cc-editing "UOHO" 4800
shot cc-options "UOHOH" 5400
shot cc-gallery "UOHOLDDDDO" 5800
shot settings "UODDDDRO" 4400
shot recents "B" 3800
shot settings-display "UODDDDRODO" 4600
shot settings-remove "UODDDDRODDDDDDDDO" 5800
shot last-row "DD" 3500
shot move-hint "H.O" 3800
shot pointer-settings "MCM" 4200 "1790,80 960,600"
shot crossfade "R" 1590
shot pointer-then-keys "MUC" 4200 "1660,850"
service HOMEPLUS_PREVIEW_PREFS='{"reduceTransparency":true}'
shot reduce-transparency "UODDDDRO" 4400
# app tiles: dark tiles; glass over the grid's background; Settings -> Display & OLED -> App Tiles, Tinted's
# colours focused and one step on
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_PREFS='{"iconLook":"dark"}'
shot icons-dark "" 2500
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_PREFS='{"iconLook":"clear"}'
shot icons-clear "DR" 3000
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_PREFS='{"iconLook":"tinted","iconTint":"Teal"}'
shot icons-tinted "UODDDDRODODDDDDODR" 7200
# media servers: Jellyfin's and Plex's Continue Watching, and a sign-in with a QR code
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_AMBIENT=1 HOMEPLUS_PREVIEW_CC='[{"id":"power","size":"small"},{"id":"ambientLight","size":"wide"},{"id":"screenCapture","size":"wide"},{"id":"soundMode","size":"wide"},{"id":"brightness","size":"wide"},{"id":"pictureMode","size":"wide"},{"id":"toneMapping","size":"wide"},{"id":"sleepTimer","size":"small"},{"id":"recents","size":"small"},{"id":"settings","size":"small"}]'
shot cc-custom "UO" 3600
# with LG Input Hook (the Control Center over other apps) and PicCap: Settings -> Control Center
mkdir -p "$ASSETS/lginputhook"
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_AMBIENT=1 HOMEPLUS_INPUTHOOK_BINDS="$ASSETS/lginputhook/keybinds.json"
shot settings-cc "UODDDDRODDO" 4600
# Add a Control with its first group's row full (the C3's layout): the next group goes under it
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_CC='[{"id":"power","size":"large"},{"id":"network","size":"wide"},{"id":"screenOff","size":"wide"},{"id":"sound","size":"wide"},{"id":"sleepTimer","size":"wide"},{"id":"pictureMode","size":"wide"},{"id":"eyeComfort","size":"wide"},{"id":"brightness","size":"wide"},{"id":"inputs","size":"small"},{"id":"accessibility","size":"small"},{"id":"settings","size":"small"}]'
shot cc-gallery-groups "UOHOLDDDDRO" 6000
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_MEDIA=1
shot jellyfin "" 4500
shot plex "R" 4500
# signing out asks first, and the question opens on Cancel: a stray OK keeps the sign-in (Up twice: Jellyfin's
# Continue Watching, then the clock)
shot signout-ask "UUODDDDRODDDDDOO" 6400
# streaming apps: what's popular on them (a mock TMDB), turning, and stepping through with Up
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_TMDB=1
shot popular "" 5200
shot popular-browse "URR" 5600
# back from an app opened from Stremio's tile (the Home button, a moment later): as it was left, Stremio's tile
# focused and its Continue Watching in the Top Shelf, in on the first frame back (kept while hidden), which comes
# into focus from blurred and dim (the shot is that first frame: this machine's frames can take longer than the
# 700 ms; Y is at 4950). The keys wait a moment for the app list: on a busy machine a Right came before it, and
# the scene opened another app.
shot back-focus "......RR......O.X......Y" 4960
shot back-home "......RR......O.X......Y" 5800
# ...and away long enough that it let go of its render targets (HOMEPLUS_TARGETS_KEPT_MS: 30 s on the TV): the
# first frame back makes them again, and the home screen is as it is after a moment away (check-screens.py)
export HOMEPLUS_TARGETS_KEPT_MS=300
shot back-later "......RR......O.X......Y" 5800
unset HOMEPLUS_TARGETS_KEPT_MS
# at rest, the loop sleeps (the TV's CPU and GPU are PicCap's and HyperHDR's too), also with Stremio's
# Continue Watching turning its pages
service HOMEPLUS_PREVIEW=1
rest stremio "RR"
# no app list (webOS lists none): after its tries the launcher says it couldn't start, and rests
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_NO_APPS=1
shot no-apps "" 28000
rest no-apps ""
service HOMEPLUS_PREVIEW=1 HOMEPLUS_PREVIEW_MEDIA=signin
shot signin-plex "UODDDDRODDDDDODDDO" 6400
service HOMEPLUS_PREVIEW=1
# first run: a fresh install welcomes you, then asks about the Home button, Continue Watching (Stremio's
# signed in; Jellyfin and Plex can be; Trakt's and TMDB's keys are pasted on a phone, from a QR code) and the app
# tiles (Dark picked: the tiles behind change), and ends on a tip or two; with LG Input Hook, about the remote's
# buttons too. An unelevated service with no Homebrew Channel (no luna-send here) shows that screen
service HOMEPLUS_PREVIEW_FRESH=1 HOMEPLUS_KEYS_HOST=192.168.1.23
shot first-run-welcome "" 3000
shot first-run-home "O" 3300
shot first-run-accounts "OO" 3600
shot first-run-add-keys "OODDDDO" 4600
shot first-run-tiles "OODDDDDODO" 4600
shot first-run-done "OODDDDDODDDDO" 4900
service HOMEPLUS_PREVIEW_FRESH=1 HOMEPLUS_INPUTHOOK_BINDS="$ASSETS/lginputhook/keybinds.json"
shot first-run-keys "OO" 3600
service HOMEPLUS_PREVIEW_UNROOTED=1
shot first-run-no-hbc "" 6000
# the old Home+ (com.github.pmiraka.homeplus, Home+'s id up to 0.14.0) still installed: its service answers
service HOMEPLUS_PREVIEW_OLD=1
shot old-home-plus "" 3000
