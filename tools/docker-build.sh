#!/bin/sh
# Builds dist/*.ipk inside Docker, so it works on macOS too (no Linux toolchain needed).
set -eu
cd "$(dirname "$0")/.."
docker build -t homeplus-build docker
metadata=$(mktemp /tmp/homeplus-update-info.XXXXXX)
trap 'rm -f "$metadata"' EXIT HUP INT TERM
if [ -n "${HOMEPLUS_UPDATE_INFO:-}" ]; then
  cp "$HOMEPLUS_UPDATE_INFO" "$metadata"
else
  docker run --rm -v "$PWD":/src homeplus-build node tools/update-provenance.js > "$metadata"
fi
# The container runs as root; hand the results back to you.
docker run --rm -v "$PWD":/src -v "$metadata":/update-info-build.json:ro \
  -e HOMEPLUS_UPDATE_INFO=/update-info-build.json homeplus-build \
  sh -c "tools/build-release.sh && chown -R $(id -u):$(id -g) dist native/build build"
