# tebox-cast against project FFmpeg only (thirdparty/ffmpeg).
# No Homebrew / system libav* — required for a self-contained package.
# CAST_SYSTEM_FFMPEG=1 builds against the distro FFmpeg instead (containers).

ROOT := $(abspath ../../..)
OUT := $(ROOT)/out/service
PREFIX_FFMPEG ?= $(ROOT)/out/cast-ffmpeg
HOST_ID := $(shell uname -s | tr A-Z a-z)-$(shell uname -m | sed 's/aarch64/arm64/')
PREBUILT_FFMPEG := $(ROOT)/prebuilts/host/$(HOST_ID)/ffmpeg

ifeq ($(wildcard $(PREBUILT_FFMPEG)/lib/pkgconfig/libavcodec.pc),)
  FFMPEG_HOME := $(PREFIX_FFMPEG)
else
  FFMPEG_HOME := $(PREBUILT_FFMPEG)
endif

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -Iinclude -Isrc
LDFLAGS ?=

PKG_CONFIG ?= pkg-config
ifeq ($(CAST_SYSTEM_FFMPEG),1)
# Distro FFmpeg (the CIX R1P container image ships Debian's libav*).
AV_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil libswscale)
AV_LIBS := $(shell $(PKG_CONFIG) --libs libavcodec libavutil libswscale)
else
ifeq ($(wildcard $(FFMPEG_HOME)/lib/pkgconfig/libavcodec.pc),)
  $(error Bundled FFmpeg missing at $(FFMPEG_HOME). Run: ./scripts/build-ffmpeg-cast.sh)
endif

# Pin PKG_CONFIG_PATH for this recipe only so Homebrew cannot win.
PKG_ENV := PKG_CONFIG_PATH=$(FFMPEG_HOME)/lib/pkgconfig
AV_CFLAGS := $(shell $(PKG_ENV) $(PKG_CONFIG) --cflags libavcodec libavutil libswscale)
AV_LIBS := $(shell $(PKG_ENV) $(PKG_CONFIG) --libs libavcodec libavutil libswscale)

# Hard-pin include/lib dirs as well (pkg-config .pc may still mention brew deps).
AV_CFLAGS += -I$(FFMPEG_HOME)/include
AV_LIBS := -L$(FFMPEG_HOME)/lib -lavcodec -lswscale -lavutil
endif

ifeq ($(shell uname -s),Darwin)
  RPATH := -Wl,-rpath,@loader_path/../ffmpeg/lib -Wl,-rpath,$(FFMPEG_HOME)/lib
else
  RPATH := -Wl,-rpath,'$$ORIGIN/../ffmpeg/lib' -Wl,-rpath,$(FFMPEG_HOME)/lib
endif

SRCS := \
	src/protocol.c \
	src/encoder.c \
	src/transport.c \
	src/capture.c \
	src/control.c \
	src/v4l2enc.c \
	src/media.c \
	src/venc.c \
	src/vdec.c \
	src/main.c

OBJS := $(SRCS:src/%.c=$(OUT)/%.o)

.PHONY: all viewer clean demo

# The viewer window (tebox-cast-view) uses SDL2 and the same FFmpeg.
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)

all: $(OUT)/tebox-cast

# Desktop viewer: `make -C src/service/cast viewer`.
viewer: $(OUT)/tebox-cast-view

# FFmpeg's include dir first so a Homebrew/system FFmpeg cannot win.
$(OUT)/tebox-cast-view: tools/cast_view.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(AV_CFLAGS) $(SDL_CFLAGS) -o $@ $< -L$(FFMPEG_HOME)/lib -lavdevice -lavformat -lavcodec -lswscale -lavutil $(SDL_LIBS) $(RPATH) $(LDFLAGS)

$(OUT)/tebox-cast: $(OBJS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^ $(AV_LIBS) $(RPATH) $(LDFLAGS)

$(OUT)/%.o: src/%.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(AV_CFLAGS) -c -o $@ $<

demo: $(OUT)/tebox-cast
	DYLD_LIBRARY_PATH=$(FFMPEG_HOME)/lib:$$DYLD_LIBRARY_PATH \
	  $(OUT)/tebox-cast --demo 320x240 --soft --out $(OUT)/demo.h264 --listen 127.0.0.1:9901

clean:
	rm -rf $(OUT)/tebox-cast $(OUT)/tebox-cast-view $(OUT)/*.o $(OUT)/demo.h264
