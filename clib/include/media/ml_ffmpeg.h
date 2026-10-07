/*
 * ml_ffmpeg.h — FFmpeg allocator thin wrappers.
 *
 * These are NOT pool memory.  Every allocation here uses the FFmpeg
 * internal allocator (av_malloc / av_mallocz) and MUST be released
 * with the matching FFmpeg free function.  Never pass these pointers
 * to pool->f_free() or ml_free().
 */
#ifndef ML_FFMPEG_H
#define ML_FFMPEG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>

/* ---- raw memory (wraps av_malloc / av_mallocz → av_free) ---- */

static inline void *ml_ffmpeg_alloc(size_t size)
{
    return av_malloc(size);
}

static inline void *ml_ffmpeg_allocz(size_t size)
{
    return av_mallocz(size);
}

static inline void ml_ffmpeg_free(void *p)
{
    av_free(p);
}

/* ---- AVPacket (wraps av_packet_alloc → av_packet_free) ---- */

static inline AVPacket *ml_ffmpeg_packet_alloc(void)
{
    return av_packet_alloc();
}

static inline void ml_ffmpeg_packet_free(AVPacket **p)
{
    av_packet_free(p);
}

/* ---- AVFrame (wraps av_frame_alloc → av_frame_free) ---- */

static inline AVFrame *ml_ffmpeg_frame_alloc(void)
{
    return av_frame_alloc();
}

static inline void ml_ffmpeg_frame_free(AVFrame **f)
{
    av_frame_free(f);
}

/* ---- AVDictionary (wraps av_dict_* → av_dict_free) ---- */

static inline void ml_ffmpeg_dict_free(AVDictionary **d)
{
    av_dict_free(d);
}

#ifdef __cplusplus
}
#endif

#endif /* ML_FFMPEG_H */
