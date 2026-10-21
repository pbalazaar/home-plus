#!/bin/sh
# Builds Home+ and installs it on a rooted webOS TV over SSH (from macOS or Linux).
# usage: tools/install.sh <tv-ip> [--autostart]
#   --autostart  also links the boot script (the service does that itself once it's root)
# Installs the Home+ package (launcher + service), re-elevates the service (every install
# resets it), then opens Home+.
set -eu

USAGE='usage: tools/install.sh <tv-ip> [--autostart]'
TV=${1:?$USAGE}
AUTOSTART=${2:-}
case $AUTOSTART in '' | --autostart) ;; *) echo "$USAGE" >&2; exit 2 ;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SSH_OPTS='-o ConnectTimeout=8'
DIR=/tmp/homeplus-install # on the TV; removed again when the install is done

# The native binary needs the webOS toolchain (Linux or Docker); package it when it has been built.
node "$ROOT/tools/build.mjs"
APP=$(ls -t "$ROOT"/dist/com.github.pbalazaar.homeplus_*_all.ipk | head -1)
NATIVE=

echo "→ copying $(basename "$APP")${NATIVE:+ and $(basename "$NATIVE")} to $TV"
ssh $SSH_OPTS "root@$TV" "rm -rf $DIR && mkdir -p $DIR && cat > $DIR/install.sh" <<'REMOTE'
# Runs on the TV (busybox sh): sh install.sh [--autostart] <app.ipk> [<native.ipk>]
DIR=$(dirname "$0")
APPS=/media/developer/apps
SVC=com.github.pbalazaar.homeplus.service
ELEVATE=$APPS/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service
AUTOSTART=
[ "$1" = --autostart ] && { AUTOSTART=1; shift; }
failed=0
ok() { echo "   ✓ $*"; }
fail() { echo "   ✗ $*"; failed=1; }

# luna-send sometimes loses its replies when its output isn't a terminal, so every call
# runs in one, made by script(1). JSON arguments must not contain single quotes.
command -v script >/dev/null || { echo "   ✗ script(1) is missing on the TV"; exit 1; }
luna() { # luna <uri> <json> [timeout-ms]: prints the reply
  script -q -c "exec luna-send -n 1 -w ${3:-8000} $1 '$2'" /dev/null </dev/null | tr -d '\r'
}
field() { sed -n "s/.*\"$1\":\"*\([^,\"}]*\).*/\1/p" | tail -1; } # echo <json> | field <name>

# Never pkill -f here: the pattern could match this SSH session's own command line.
stop_service() {
  for p in $(pgrep -f pbalazaar.homeplus.service); do [ "$p" != "$$" ] && kill "$p" 2>/dev/null; done
  i=0
  while pgrep -f pbalazaar.homeplus.service >/dev/null; do
    [ $i -ge 10 ] && { fail "couldn't stop $SVC"; return 1; }
    sleep 0.3; i=$((i + 1))
  done
}

label_of() { name=${1%_all.ipk}; echo "${name%_*} ${name##*_}"; } # <id>_<version>_all.ipk -> "<id> <version>"

# install_ipk <file in $DIR>: installs it, then checks every file against the package
install_ipk() {
  label=$(label_of "$1")
  work=$DIR/${1%_all.ipk}
  if ! err=$(mkdir -p "$work/data" && cd "$work" && ar x "$DIR/$1" 2>&1 && tar -xzf data.tar.gz -C data 2>&1); then
    fail "$label: can't read $1 ($err)"
    return
  fi
  (cd "$work/data" && find . -type f | sort | while read -r f; do sha256sum "$f"; done) >"$work/sums"

  script -q -c "exec luna-send -i -w 130000 luna://com.webos.appInstallService/dev/install \
    '{\"id\":\"com.ares.defaultName\",\"ipkUrl\":\"$DIR/$1\",\"subscribe\":true}'" /dev/null \
    </dev/null >"$work/log" 2>&1 &
  pid=$!
  state=none
  i=0
  while :; do
    kill -0 $pid 2>/dev/null; running=$? # checked before reading, so a finished log is read whole
    case $(cat "$work/log") in
      *'"state":"installed"'*) state=installed; break ;;
      *'"state":"install failed"'* | *'"errorCode"'* | *'"returnValue":false'*) state=failed; break ;;
    esac
    [ $running = 0 ] && [ $i -lt 240 ] || break
    sleep 0.5; i=$((i + 1))
  done
  pkill -P $pid 2>/dev/null # luna-send never ends the subscription by itself
  wait $pid 2>/dev/null

  case $state in
    installed)
      total=$(wc -l <"$work/sums")
      good=$(cd $APPS && sha256sum -c "$work/sums" 2>/dev/null | grep -c ': OK$')
      if [ "$total" -gt 0 ] && [ "$good" = "$total" ]; then
        ok "$label installed, $total files verified"
      else
        fail "$label: installed, but only $good of $total files match the package:"
        (cd $APPS && sha256sum -c "$work/sums" 2>&1 | grep -v ': OK$' | head -5 | sed 's/^/      /')
      fi ;;
    failed)
      last=$(tr -d '\r' <"$work/log" | grep -E '"state":"install failed"|"errorCode"|"returnValue":false' | tail -1)
      why=$(echo "$last" | field reason)
      [ -n "$why" ] || why=$(echo "$last" | field errorText)
      fail "$label: install failed (${why:-$last})" ;;
    *)
      last=$(tr -d '\r' <"$work/log" | grep . | tail -1 | cut -c1-300)
      fail "$label: no result from the installer after $((i / 2)) s${last:+; its last message: $last}" ;;
  esac
}

echo "   closing Home+ and stopping its service"
for id in com.github.pbalazaar.homeplus; do
  luna luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$id\"}" >/dev/null
done
stop_service

# Home+ up to 0.14.0 had another id: it's another app to webOS, and its service would take Home+'s port.
# It's uninstalled as a plain uninstall does it (not Remove Home+): settings, sign-ins and binds stay.
OLD=com.github.pmiraka.homeplus
if [ -d "$APPS/usr/palm/applications/$OLD" ]; then
  luna luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$OLD\"}" >/dev/null
  for p in $(pgrep -f pmiraka.homeplus.service); do [ "$p" != "$$" ] && kill "$p" 2>/dev/null; done
  luna luna://com.webos.appInstallService/dev/remove "{\"id\":\"$OLD\"}" 30000 >/dev/null
  i=0
  while [ -d "$APPS/usr/palm/applications/$OLD" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
  if [ -d "$APPS/usr/palm/applications/$OLD" ]; then fail "couldn't uninstall the old Home+ ($OLD)"; else ok "the old Home+ ($OLD) uninstalled; its settings stay"; fi
fi

for ipk; do # the app (with the service) first, then the native front end
  if [ $failed = 0 ]; then install_ipk "$ipk"; else echo "   - $(label_of "$ipk"): skipped"; fi
done

if [ ! -x "$ELEVATE" ]; then
  fail "elevate-service not found: is Homebrew Channel installed and up to date?"
elif out=$("$ELEVATE" $SVC 2>&1); then
  ok "service elevated"
else
  fail "elevate-service failed:"
  echo "$out" | tail -5 | sed 's/^/      /'
fi

# Restart the service so the running copy is the new, elevated one.
stop_service
reply=$(luna luna://$SVC/ping '{}' 15000)
uid=$(echo "$reply" | field uid)
if [ "$uid" = 0 ]; then
  ok "service $(echo "$reply" | field version) is running as root"
else
  fail "service didn't start as root: ${reply:-no answer to ping}"
fi

if [ -n "$AUTOSTART" ]; then # a link, as the service makes it (Homebrew Channel's rules: not a copy)
  mkdir -p /var/lib/webosbrew/init.d &&
    ln -sf $APPS/usr/palm/applications/com.github.pbalazaar.homeplus/init.d/50-homeplus /var/lib/webosbrew/init.d/50-homeplus &&
    ok "boot script linked" || fail "couldn't link the boot script"
fi

app=com.github.pbalazaar.homeplus
reply=$(luna luna://com.webos.applicationManager/launch "{\"id\":\"$app\"}")
i=0
while [ $i -lt 20 ]; do
  fg=$(luna luna://com.webos.applicationManager/getForegroundAppInfo '{}' | field appId)
  [ "$fg" = $app ] && break
  sleep 0.5; i=$((i + 1))
done
if [ "$fg" = $app ]; then ok "$app is on screen"; else fail "launched $app, but ${fg:-nothing} is on screen ($reply)"; fi

cd / && rm -rf "$DIR"
if [ $failed = 0 ]; then echo "→ done."; else echo "→ FAILED (see ✗ above)"; exit 1; fi
REMOTE
scp -q $SSH_OPTS "$APP" ${NATIVE:+"$NATIVE"} "root@$TV:$DIR/"

echo "→ installing on $TV"
ssh -n $SSH_OPTS "root@$TV" "sh $DIR/install.sh $AUTOSTART $(basename "$APP") ${NATIVE:+$(basename "$NATIVE")}"
