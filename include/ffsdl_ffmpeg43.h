/* FFmpeg compatibility policy owned by the ffsdl demo, not by clib. */
#ifndef FFSDL_FFMPEG43_H
#define FFSDL_FFMPEG43_H

#include <libavcodec/version.h>
#include <libavformat/version.h>
#include <libavutil/version.h>

#if LIBAVCODEC_VERSION_MAJOR < 58 || \
    LIBAVFORMAT_VERSION_MAJOR < 58 || \
    LIBAVUTIL_VERSION_MAJOR < 56
#error "ffsdl requires FFmpeg 4.3 ABI or newer"
#endif

#endif
