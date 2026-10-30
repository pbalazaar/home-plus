#!/bin/sh
# Runs Home+'s checks; CI runs all of them but the screens on every pull request.
#   tools/test.sh            the service's tests: unit tests and its HTTP API (seconds; Node 18+)
#   tools/test.sh native     the launcher's own tests (native/test/: its requests, pictures and media sources), as they are
#                            and under ASan and UBSan; on Linux with the preview packages, else in Docker (under a minute)
#   tools/test.sh screens    renders every launcher screen and checks them; they land in build/screens/
#                            (on Linux with the preview packages, else in Docker; about 6 minutes). Not in
#                            CI: run it when a change touches what the launcher draws
#   tools/test.sh qml        loads the Control Center's QML (native/qml) and drives it, as LG's
#                            compositor would (on Linux with the preview packages, else in Docker)
#   tools/test.sh package    builds the .ipk for the TV and checks it (WEBOS_SDK, else Docker)
#   tools/test.sh all        all five
# On Windows, run it in WSL. (In PowerShell, the service's tests are: node --test "test/*.test.js")
set -eu
cd "$(dirname "$0")/.."

service() {
  echo "== service tests"
  node --test test/*.test.js
}

native() {
  echo "== the launcher's own tests"
  run='make -s -C native test sanitize'
  if [ "$(uname)" = Linux ] && [ -f /usr/include/SDL2/SDL.h ] && python3 -c 'import PIL' 2>/dev/null; then
    sh -c "$run"
  elif command -v docker >/dev/null; then
    docker build -q -t homeplus-preview tools/native-preview >/dev/null
    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src homeplus-preview sh -c "$run"
  else
    echo "needs Docker, or Linux with: libsdl2-dev python3-pil" >&2
    exit 1
  fi
}

screens() {
  echo "== launcher screens"
  rm -rf build/screens
  mkdir -p build/screens
  run='tools/native-preview/run.sh && python3 tools/native-preview/check-screens.py build/screens'
  if [ "$(uname)" = Linux ] && command -v xvfb-run >/dev/null && python3 -c 'import PIL' 2>/dev/null; then
    HOMEPLUS_PREVIEW_OUT=build/screens sh -c "$run"
  elif command -v docker >/dev/null; then
    docker build -q -t homeplus-preview tools/native-preview >/dev/null
    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src -e HOMEPLUS_PREVIEW_OUT=build/screens homeplus-preview sh -c "$run"
  else
    echo "needs Docker, or Linux with: libsdl2-dev xvfb xauth libgles2 libegl1 libgl1-mesa-dri python3-pil nodejs" >&2
    exit 1
  fi
  echo "   the screens: build/screens/"
}

qml() {
  echo "== the Control Center's QML"
  if command -v qmlscene >/dev/null && command -v xvfb-run >/dev/null; then
    tools/qml-check/run.sh
  elif command -v docker >/dev/null; then
    docker build -q -t homeplus-preview tools/native-preview >/dev/null
    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src homeplus-preview tools/qml-check/run.sh
  else
    echo "needs qmlscene and xvfb-run, or Docker" >&2
    exit 1
  fi
}

package() {
  echo "== package"
  if [ -n "${WEBOS_SDK:-}" ]; then tools/build-release.sh; else tools/docker-build.sh; fi
  node tools/check-package.mjs
}

case ${1:-service} in # (not "a && b": sh ignores set -e inside functions run that way)
  service | native | screens | qml | package) "${1:-service}" ;;
  all) service; native; screens; qml; package ;;
  *) sed -n '2,12p' "$0" >&2; exit 2 ;;
esac
