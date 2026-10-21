# The desktop preview's environment: builds the launcher for Linux and renders its screens with
# Mesa under Xvfb (run.sh), and loads the Control Center's QML (tools/qml-check), on any machine with
# Docker. tools/test.sh uses it when it isn't on Linux.
#   docker build -t homeplus-preview tools/native-preview
#   docker run --rm -v "$PWD":/src homeplus-preview tools/native-preview/run.sh
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make libsdl2-dev xvfb xauth libgles2 libegl1 \
      libgl1-mesa-dri python3 python3-pil nodejs \
      qmlscene qml-module-qtquick2 qml-module-qtgraphicaleffects qml-module-qtmultimedia \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
