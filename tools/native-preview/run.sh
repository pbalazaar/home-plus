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
# back from an app opened from Stremio's tile (the Home button, a moment later): at the top again, Netflix's
# popular titles in the Top Shelf, already in on the first frame back (they're loaded while hidden)
shot back-home "RR......O.X......Y" 4100
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
# first run: a fresh install asks about the Home button; an unelevated service
# with no Homebrew Channel (no luna-send here) shows that screen
service HOMEPLUS_PREVIEW_FRESH=1
shot first-run-home "" 3000
service HOMEPLUS_PREVIEW_UNROOTED=1
shot first-run-no-hbc "" 6000
# the old Home+ (com.github.pmiraka.homeplus, Home+'s id up to 0.14.0) still installed: its service answers
service HOMEPLUS_PREVIEW_OLD=1
shot old-home-plus "" 3000
