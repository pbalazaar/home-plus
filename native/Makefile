# Home+ native front end.
#   make            -> build/host/homeplus   (desktop build, for previews)
#   make webos      -> build/webos/homeplus  (32-bit ARM, links the TV's SDL2 2.0.14)
#
# Compiled against vendored SDL 2.0.14 headers so the binary only uses
# functions the TV's libSDL2 actually has.

SRC := src/main.c src/util.c src/tiles.c src/draw.c src/sheet.c src/recents.c src/cc.c src/cc_list.c src/gfx.c src/render.c src/sound.c src/net.c src/luna.c src/preview.c third_party/cJSON.c
CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -Ithird_party -Ithird_party/SDL2 -D_REENTRANT

WEBOS_SDK ?= $(HOME)/toolchain/arm-webos-linux-gnueabi_sdk-buildroot
WEBOS_CC := $(WEBOS_SDK)/bin/arm-webos-linux-gnueabi-gcc
WEBOS_SYSROOT = $(shell $(WEBOS_CC) -print-sysroot)

HOST_SDL ?= $(firstword $(wildcard /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 \
                                   /usr/lib/libSDL2-2.0.so.0))

all: build/host/homeplus

build/host/homeplus: $(SRC) src/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(HOST_SDL) -lm -lpthread -ldl
	@rm -rf $(@D)/assets && cp -r assets $(@D)/assets

webos: build/webos/homeplus

build/webos/homeplus: $(SRC) src/*.h
	@mkdir -p $(@D)
	$(WEBOS_CC) $(CFLAGS) -o $@ $(SRC) \
	  -L$(WEBOS_SYSROOT)/usr/lib -lSDL2 -lm -lpthread -ldl -Wl,-rpath-link,$(WEBOS_SYSROOT)/usr/lib
	$(WEBOS_SDK)/bin/arm-webos-linux-gnueabi-strip $@
	@rm -rf $(@D)/assets && cp -r assets $(@D)/assets

clean:
	rm -rf build

.PHONY: all webos clean
