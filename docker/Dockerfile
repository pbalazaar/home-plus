# Build environment for Home+: the webOS TV toolchain plus make and Node.
# Works on Intel and Apple Silicon hosts (the toolchain ships for both).
#
#   docker build -t homeplus-build docker
#   docker run --rm -v "$PWD":/src homeplus-build        # -> dist/*.ipk
#
# Or just run tools/docker-build.sh, which does both.
FROM debian:bookworm-slim

# openlgtv/buildroot-nc4 release; its SDK targets webOS's 32-bit ARM userland. Its files' SHA-256s
# are in toolchain.sha256, and change with it (keep .github/workflows/ci.yml in step too).
ARG SDK_RELEASE=2026.08-webos
ARG TARGETARCH

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl make nodejs file \
 && rm -rf /var/lib/apt/lists/*

COPY toolchain.sha256 /tmp/
RUN set -e; \
    case "${TARGETARCH:-$(dpkg --print-architecture)}" in \
      amd64) host=x86_64 ;; \
      arm64) host=aarch64 ;; \
      *) echo "unsupported build host: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    f="arm-webos-linux-gnueabi_sdk-buildroot-${host}.tar.gz"; \
    cd /tmp; \
    curl -fsSLO "https://github.com/openlgtv/buildroot-nc4/releases/download/${SDK_RELEASE}/$f"; \
    grep " $f\$" toolchain.sha256 | sha256sum -c -; \
    tar xzf "$f" -C /opt; \
    rm "$f" toolchain.sha256; \
    /opt/arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh

ENV WEBOS_SDK=/opt/arm-webos-linux-gnueabi_sdk-buildroot
WORKDIR /src
CMD ["tools/build-release.sh"]
