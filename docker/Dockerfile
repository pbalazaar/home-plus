# Build environment for Home+: the webOS TV toolchain plus GNU tar and Node.
# Works on Intel and Apple Silicon hosts (the toolchain ships for both).
#
#   docker build -t homeplus-build docker
#   docker run --rm -v "$PWD":/src homeplus-build        # -> dist/*.ipk
#
# Or just run tools/docker-build.sh, which does both.
FROM debian:bookworm-slim

# openlgtv/buildroot-nc4 release; its SDK targets webOS's 32-bit ARM userland
ARG SDK_RELEASE=2026.08-webos
ARG TARGETARCH

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl make nodejs tar gzip file \
 && rm -rf /var/lib/apt/lists/*

RUN set -e; \
    case "${TARGETARCH:-$(dpkg --print-architecture)}" in \
      amd64) host=x86_64 ;; \
      arm64) host=aarch64 ;; \
      *) echo "unsupported build host: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL "https://github.com/openlgtv/buildroot-nc4/releases/download/${SDK_RELEASE}/arm-webos-linux-gnueabi_sdk-buildroot-${host}.tar.gz" \
      | tar xz -C /opt; \
    /opt/arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh

ENV WEBOS_SDK=/opt/arm-webos-linux-gnueabi_sdk-buildroot
WORKDIR /src
CMD ["tools/build-release.sh"]
