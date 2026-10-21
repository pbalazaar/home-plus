#!/bin/sh
# Writes native/qml/glyphs/*.png (Home+'s glyphs, white, 128 px) for the Control Center over other
# apps. Runs in the preview container (tools/native-preview/Dockerfile), or on Linux with SDL2's headers.
set -e
cd "$(dirname "$0")/.."
if [ -z "$IN_PREVIEW" ] && command -v docker >/dev/null && [ "$(uname)" != Linux ]; then
  exec docker run --rm -e IN_PREVIEW=1 -v "$PWD:/src" -w /src homeplus-preview:latest sh tools/make-glyphs.sh
fi
out=$(mktemp -d)
cc -O2 -Inative/src -Inative/third_party $(sdl2-config --cflags) tools/native-preview/glyphs.c -lm -o "$out/glyphs"
mkdir -p native/qml/glyphs
"$out/glyphs" native/qml/glyphs 128
rm -rf "$out"
echo "wrote native/qml/glyphs"
