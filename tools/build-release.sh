#!/bin/sh
# Builds the native launcher for the TV and packages dist/*.ipk.
# Needs WEBOS_SDK (the openlgtv/buildroot-nc4 toolchain), make, GNU tar and Node 16+.
# Used by docker/Dockerfile and .github/workflows/release.yml.
set -eu
cd "$(dirname "$0")/.."
: "${WEBOS_SDK:?set WEBOS_SDK to the arm-webos-linux-gnueabi_sdk-buildroot directory}"

make -C native webos WEBOS_SDK="$WEBOS_SDK"
rm -f dist/*.ipk
node tools/build.mjs native
ls -l dist/*.ipk
