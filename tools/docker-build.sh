#!/bin/sh
# Builds dist/*.ipk inside Docker, so it works on macOS too (no Linux toolchain needed).
set -eu
cd "$(dirname "$0")/.."
docker build -t homeplus-build docker
# The container runs as root; hand the results back to you.
docker run --rm -v "$PWD":/src homeplus-build \
  sh -c "tools/build-release.sh && chown -R $(id -u):$(id -g) dist native/build build"
