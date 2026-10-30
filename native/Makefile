# Home+ native front end.
#   make            -> build/host/homeplus   (desktop build, for previews)
#   make webos      -> build/webos/homeplus  (32-bit ARM, links the TV's SDL2 2.0.14)
#   make test       -> its own tests, on the desktop (test/; their pictures need python3 with PIL)
#   make sanitize   -> the same tests under AddressSanitizer and UBSan: the first fault fails them
#
# Compiled against vendored SDL 2.0.14 headers so the binary only uses
# functions the TV's libSDL2 actually has.

SRC := src/main.c src/util.c src/tiles.c src/icon.c src/draw.c src/sheet.c src/recents.c src/cc.c src/cc_list.c src/shelf.c src/apps.c src/setup.c src/accounts.c src/settings.c src/actions.c src/input.c src/idle.c src/results.c src/perf.c src/gfx.c src/render.c src/sound.c src/net.c src/luna.c src/preview.c third_party/cJSON.c
CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -Ithird_party -Ithird_party/SDL2 -D_REENTRANT

WEBOS_SDK ?= $(HOME)/toolchain/arm-webos-linux-gnueabi_sdk-buildroot
WEBOS_CC := $(WEBOS_SDK)/bin/arm-webos-linux-gnueabi-gcc
WEBOS_SYSROOT = $(shell $(WEBOS_CC) -print-sysroot)

HOST_SDL ?= $(firstword $(wildcard /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 \
                                   /usr/lib/libSDL2-2.0.so.0))

TEST_OUT := build/host
TEST_FLAGS :=

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

test: $(TEST_OUT)/net-test $(TEST_OUT)/icon-test $(TEST_OUT)/util-test
	@python3 test/make-fixtures.py build/test
	$(TEST_OUT)/net-test build/test
	$(TEST_OUT)/icon-test
	$(TEST_OUT)/util-test

$(TEST_OUT)/net-test: test/net_test.c src/net.c src/luna.c src/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_FLAGS) -Isrc -o $@ test/net_test.c src/net.c src/luna.c $(HOST_SDL) -lm -lpthread -ldl

$(TEST_OUT)/icon-test: test/icon_test.c src/icon.c src/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_FLAGS) -Isrc -o $@ test/icon_test.c src/icon.c -lm

$(TEST_OUT)/util-test: test/util_test.c src/util.c src/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_FLAGS) -Isrc -o $@ test/util_test.c src/util.c third_party/cJSON.c -lm

sanitize:
	@UBSAN_OPTIONS=print_stacktrace=1 $(MAKE) test TEST_OUT=build/sanitize \
	  TEST_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined"

clean:
	rm -rf build

.PHONY: all webos test sanitize clean
