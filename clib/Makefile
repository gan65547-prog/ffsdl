# Minimal clib extraction for the RKMPP + RGA + SDL2 RTSP demo.

CC ?= gcc
AR ?= ar
PROJECT_ROOT ?= $(abspath $(CURDIR)/..)
THIRD_PARTY_ROOT ?= $(CURDIR)/../third_party
FFMPEG_ROOT ?=
FFMPEG_VERSION ?= 4.3
SDL2_ROOT ?=

CFLAGS := -D_GNU_SOURCE -DML_CAMERA_HAS_RTSP -fPIC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -I$(CURDIR)/include -I$(PROJECT_ROOT)/include
LDFLAGS :=
LIBS := -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale)
  FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale)
  SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
  SDL_LIBS := $(shell pkg-config --libs sdl2)
  RGA_SRC := src/platform/ml_rga_stub.c
  SHARED := libclib.dylib
  SHARED_FLAGS := -dynamiclib -Wl,-install_name,@rpath/libclib.dylib
else
LDFLAGS += -Wl,-rpath,'$$ORIGIN/../third_party/ffmpeg-$(FFMPEG_VERSION)/lib' \
             -Wl,-rpath,'$$ORIGIN/../third_party/sdl2/lib' \
             -Wl,-rpath,'$$ORIGIN/../third_party/lib/aarch64-linux-gnu'
  ifneq ($(wildcard $(FFMPEG_ROOT)/include/libavformat/avformat.h),)
    FFMPEG_CFLAGS := -I$(FFMPEG_ROOT)/include
    FFMPEG_LIBS := $(FFMPEG_ROOT)/lib/libavformat.so \
                   $(FFMPEG_ROOT)/lib/libavcodec.so \
                   $(FFMPEG_ROOT)/lib/libavutil.so \
                   $(FFMPEG_ROOT)/lib/libswscale.so
  else ifneq ($(wildcard $(THIRD_PARTY_ROOT)/include/ffmpeg),)
    FFMPEG_CFLAGS := -I$(THIRD_PARTY_ROOT)/include/ffmpeg
    FFMPEG_LIBS := -L$(THIRD_PARTY_ROOT)/lib/linux-aarch64/ffmpeg -lavformat -lavcodec -lavutil -lswscale
  else
    FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale)
    FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale)
  endif
  ifneq ($(wildcard $(SDL2_ROOT)/include/SDL2/SDL.h),)
    SDL_CFLAGS := -I$(SDL2_ROOT)/include/SDL2 -I$(SDL2_ROOT)/include/aarch64-linux-gnu
    SDL_LIBS := $(SDL2_ROOT)/lib/libSDL2.so
  else
    SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
    SDL_LIBS := $(shell pkg-config --libs sdl2)
  endif
  RGA_SRC := src/platform/ml_rga.c
  RGA_CFLAGS := -I$(THIRD_PARTY_ROOT)/include/rga -I$(THIRD_PARTY_ROOT)/include/mpp
  RGA_LIBS := -L$(THIRD_PARTY_ROOT)/lib/aarch64-linux-gnu -lrga \
              -L$(THIRD_PARTY_ROOT)/lib/aarch64-linux-gnu -lrockchip_mpp
  SHARED := libclib.so
  SHARED_FLAGS := -shared -Wl,-soname,libclib.so
endif

SOURCES := src/memory/ml_pool.c \
           src/memory/ml_frame.c src/io/ml_camera.c \
           src/io/ml_camera_rtsp.c src/io/ml_mpp_decoder.c src/io/ml_camera_stub.c \
           src/platform/ml_display_sdl.c $(RGA_SRC)
OBJECTS := $(SOURCES:src/%.c=build/%.o)

all: $(SHARED)

$(SHARED): $(OBJECTS)
	$(CC) $(SHARED_FLAGS) -o $@ $^ $(LDFLAGS) $(FFMPEG_LIBS) $(SDL_LIBS) $(RGA_LIBS) $(LIBS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) $(SDL_CFLAGS) $(RGA_CFLAGS) -c $< -o $@

.PHONY: all clean
clean:
	$(RM) -r build libclib.so libclib.dylib libclib.a
