# The desktop preview's environment: builds the launcher for Linux and renders its screens with
# Mesa under Xvfb (run.sh), on any machine with Docker. tools/test.sh uses it when it isn't on Linux.
#   docker build -t homeplus-preview tools/native-preview
#   docker run --rm -v "$PWD":/src homeplus-preview tools/native-preview/run.sh
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make libsdl2-dev xvfb xauth libgles2 libegl1 \
      libgl1-mesa-dri python3 python3-pil nodejs \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
