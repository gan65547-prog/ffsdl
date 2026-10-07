/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_camera_rtsp.c — RTSP network camera backend.
 *
 * Uses FFmpeg libavformat/libavcodec to pull H.264/H.265 streams
 * from RTSP cameras.  On Rockchip platforms, tries h264_rkmpp for
 * hardware decoding and RGA for NV12→BGR color conversion.
 *
 * Features:
 *   - h264_rkmpp hardware decode with software fallback
 *   - RGA-accelerated NV12→BGR color conversion (with swscale fallback)
 *   - Low-latency RTSP options (nobuffer, low_delay, max_delay)
 *   - Auto-reconnect with exponential backoff
 *   - Optional FPS limiting
 */

#include "io/ml_camera_private.h"
#include "memory/ml_frame.h"
#include "memory/ml_pool.h"
#include "media/ml_ffmpeg.h"
#include "platform/ml_rga.h"
#include "ffsdl_ffmpeg43.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── internal struct ──────────────────────────────────────────────────── */

/* ── forward decl ─────────────────────────────────────────────────────── */

static void *rtsp_thread(void *arg);

/* Frame release callback: frees per-read buffer allocated for UAF safety
 * (see clib audit M4 — internal rga_cvt_buf / rgb_buf are reused each frame). */
static void rtsp_frame_release(void *owner, ml_frame_t *f)
{
    (void)owner;
    if (f) {
        for (int i = 0; i < 3; i++) {
            if (f->data[i]) ml_ffmpeg_free(f->data[i]);
        }
    }
}

/* ── RGA color conversion (NV12 → BGR) ────────────────────────────────── */

static int rga_nv12_to_bgr(ml_camera_t *cam)
{
    int w = cam->codec_ctx->width;
    int h = cam->codec_ctx->height;
    int bgr_size = w * h * 3;

    /* allocate BGR output buffer if needed */
    if (!cam->rga_cvt_buf || cam->rga_cvt_size < bgr_size) {
        ml_ffmpeg_free(cam->rga_cvt_buf);
        cam->rga_cvt_buf = ml_ffmpeg_alloc(bgr_size);
        if (!cam->rga_cvt_buf)
            return -1;
        cam->rga_cvt_size = bgr_size;
    }

    /* build continuous NV12 buffer from AVFrame planes */
    int y_size  = cam->dec_frame->linesize[0] * h;
    int uv_size = cam->dec_frame->linesize[1] * h / 2;

    /* RGA works with continuous buffers — copy Y and UV */
    uint8_t *nv12_buf = (uint8_t *)ml_ffmpeg_alloc(y_size + uv_size);
    if (!nv12_buf)
        return -1;

    int i;
    for (i = 0; i < h; i++)
        memcpy(nv12_buf + i * w,
               cam->dec_frame->data[0] + i * cam->dec_frame->linesize[0], w);
    for (i = 0; i < h / 2; i++)
        memcpy(nv12_buf + y_size + i * w,
               cam->dec_frame->data[1] + i * cam->dec_frame->linesize[1], w);

    int ret = ml_rga_csc(cam->rga,
                         nv12_buf, w, h, ML_RGA_FMT_NV12,
                         cam->rga_cvt_buf, ML_RGA_FMT_BGR888);
    ml_ffmpeg_free(nv12_buf);
    return ret;
}

#include <sys/stat.h>

/* check if a V4L2 decoder device node is functional.
 * On some Rockchip vendor kernels, /dev/video-dec0 is a regular file (not a
 * character device), which means the rkvdec driver didn't create a proper node.
 * Trying h264_v4l2m2m in that case scans all /dev/video* devices and fails
 * noisily — skip it. */
static int v4l2_decoder_available(void)
{
    struct stat st;
    if (stat("/dev/video-dec0", &st) != 0)
        return 0;
    if (!S_ISCHR(st.st_mode))
        return 0;
    return 1;
}

/* ── FFmpeg interrupt callback for timeout ───────────────────────────── */

static int rtsp_interrupt_cb(void *opaque)
{
    ml_camera_t *cam = (ml_camera_t *)opaque;
    struct timespec now;

    /* stop() requested thread exit — always interrupt */
    if (cam->interrupt_requested)
        return 1;

    /* deadline of 0 means infinite wait (used by sync read) */
    if (cam->deadline_us == 0)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t now_us = now.tv_sec * 1000000LL + now.tv_nsec / 1000;
    return (now_us > cam->deadline_us) ? 1 : 0;
}

/* open codec and find video stream */
static int rtsp_open(ml_camera_t *cam)
{
    AVDictionary *opts = NULL;
    AVCodecParameters *codecpar;
    int ret;

    /* RTSP input stability options.  Keep TCP for lossless transport, but do
     * not use "nobuffer" here: on 1080p streams a tiny demux/socket buffer can
     * starve H.264 reference frames and produce macroblock corruption. */
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout",       "3000000", 0);
    av_dict_set(&opts, "fflags",         "discardcorrupt", 0);
    av_dict_set(&opts, "flags",          "low_delay", 0);
    av_dict_set(&opts, "max_delay",      "500000", 0);
    av_dict_set(&opts, "analyzeduration","1000000", 0);
    av_dict_set(&opts, "buffer_size",    "4194304", 0);
    av_dict_set(&opts, "reorder_queue_size", "16", 0);

    /* pre-allocate context so we can set interrupt callback */
    cam->fmt_ctx = avformat_alloc_context();
    if (!cam->fmt_ctx) {
        ml_camera_set_err(cam, "avformat_alloc_context failed");
        return -1;
    }
    cam->fmt_ctx->interrupt_callback.callback = rtsp_interrupt_cb;
    cam->fmt_ctx->interrupt_callback.opaque   = cam;

    ret = avformat_open_input(&cam->fmt_ctx, cam->cfg.device, NULL, &opts);
    ml_ffmpeg_dict_free(&opts);
    if (ret < 0) {
        /* FFmpeg >= 4.0: avformat_open_input may leave partial allocations
         * in fmt_ctx on failure; caller must still close it. */
        avformat_close_input(&cam->fmt_ctx);
        ml_camera_set_err(cam, "avformat_open_input failed");
        return -1;
    }

    /* Drop packets marked corrupt by the demuxer.  A subscriber can attach
     * in the middle of a GOP; those packets are unusable until the next IDR
     * and must not enter the decoder's reference queue. */
    cam->fmt_ctx->flags |= AVFMT_FLAG_DISCARD_CORRUPT;

    ret = avformat_find_stream_info(cam->fmt_ctx, NULL);
    if (ret < 0) {
        ml_camera_set_err(cam, "avformat_find_stream_info failed");
        return -1;
    }


    /* Select the first video stream explicitly.  This avoids relying on
     * av_find_best_stream()'s decoder probing, which is unstable with some
     * FFmpeg 6.1/RKMPP builds during RTSP stream discovery. */
    cam->video_idx = -1;
    for (unsigned i = 0; i < cam->fmt_ctx->nb_streams; i++) {
        if (cam->fmt_ctx->streams[i] && cam->fmt_ctx->streams[i]->codecpar &&
            cam->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            cam->video_idx = (int)i;
            break;
        }
    }
    if (cam->video_idx < 0) {
        ml_camera_set_err(cam, "no video stream found");
        return -1;
    }

    /* Discard all non-video streams (audio/subtitle/etc.)
     * to prevent AAC decoder buffer stalls blocking av_read_frame. */
    for (unsigned i = 0; i < cam->fmt_ctx->nb_streams; i++) {
        if (cam->fmt_ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            cam->fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
    }

    codecpar = cam->fmt_ctx->streams[cam->video_idx]->codecpar;

    /* Prefer direct MPP on Linux. FFmpeg 4.3 remains responsible for RTSP
     * demuxing, while the board MPP ABI handles hardware decode directly.
     * Fallback priority: rkmpp > v4l2m2m > software.
     * h264_rkmpp: custom FFmpeg builds with --enable-rkmpp (RK MPP)
     * h264_v4l2m2m: upstream V4L2 mem2mem stateless decoder (kernel 5.10+). */
    cam->mpp_decoder = NULL;
    cam->hw_decoder = 0;
    cam->decoder    = NULL;

    const char *disable_hw = getenv("FFSDL_DISABLE_RKMPP");
    int allow_hw = !(disable_hw && disable_hw[0] && strcmp(disable_hw, "0") != 0);

#ifdef __linux__
    if (allow_hw && (codecpar->codec_id == AV_CODEC_ID_H264 ||
                     codecpar->codec_id == AV_CODEC_ID_HEVC)) {
        cam->mpp_decoder = ml_mpp_decoder_create(codecpar->codec_id);
    }
#endif

    if (cam->mpp_decoder) {
        cam->decoder = avcodec_find_decoder(codecpar->codec_id);
        cam->hw_decoder = 1;
    } else if (allow_hw && codecpar->codec_id == AV_CODEC_ID_H264) {
        cam->decoder = avcodec_find_decoder_by_name("h264_rkmpp");
        if (!cam->decoder && v4l2_decoder_available())
            cam->decoder = avcodec_find_decoder_by_name("h264_v4l2m2m");
    } else if (allow_hw && codecpar->codec_id == AV_CODEC_ID_HEVC) {
        cam->decoder = avcodec_find_decoder_by_name("hevc_rkmpp");
        if (!cam->decoder && v4l2_decoder_available())
            cam->decoder = avcodec_find_decoder_by_name("hevc_v4l2m2m");
    }

    if (!cam->mpp_decoder && cam->decoder) {
        cam->hw_decoder = 1;
    } else if (!cam->mpp_decoder) {
        cam->decoder = avcodec_find_decoder(codecpar->codec_id);
        cam->hw_decoder = 0;
    }

    if (!cam->decoder) {
        ml_camera_set_err(cam, "no decoder found");
        return -1;
    }

    cam->codec_ctx = avcodec_alloc_context3(cam->decoder);
    if (!cam->codec_ctx)
        return -1;

    /* multi-threaded decoding for software path */
    if (!cam->hw_decoder) {
        cam->codec_ctx->thread_count = 4;
        cam->codec_ctx->thread_type  = FF_THREAD_SLICE;
    }

    ret = avcodec_parameters_to_context(cam->codec_ctx, codecpar);
    if (ret < 0) {
        ml_camera_set_err(cam, "avcodec_parameters_to_context failed");
        return -1;
    }

    /* A new RTSP subscriber commonly joins between IDR frames.  The first
     * packets can therefore reference a frame that is not in this decoder's
     * queue, which is recoverable but otherwise produces noisy
     * "decode_slice_header"/"missing reference picture" messages.  Keep
     * corrupt access units out of the output and let the decoder resync at
     * the next keyframe instead of treating the transient as a source fault.
     */
    cam->codec_ctx->err_recognition |= AV_EF_IGNORE_ERR;
#ifdef AVDISCARD_CORRUPT
    cam->codec_ctx->skip_frame = AVDISCARD_CORRUPT;
#endif

    /* low-latency flags for software decoder */
    if (!cam->hw_decoder)
        cam->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = cam->mpp_decoder ? 0 : avcodec_open2(cam->codec_ctx, cam->decoder, NULL);
    if (ret < 0 && cam->hw_decoder) {
        /* hardware decoder failed — fall back to software */
        cam->hw_decoder = 0;
        cam->decoder    = avcodec_find_decoder(codecpar->codec_id);
        if (!cam->decoder) {
            ml_camera_set_err(cam, "no software decoder fallback");
            return -1;
        }

        avcodec_free_context(&cam->codec_ctx);
        cam->codec_ctx = avcodec_alloc_context3(cam->decoder);
        if (!cam->codec_ctx)
            return -1;

        cam->codec_ctx->thread_count = 4;
        cam->codec_ctx->thread_type  = FF_THREAD_SLICE;

        ret = avcodec_parameters_to_context(cam->codec_ctx, codecpar);
        if (ret < 0) {
            ml_camera_set_err(cam, "avcodec_parameters_to_context failed");
            return -1;
        }

        cam->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ret = avcodec_open2(cam->codec_ctx, cam->decoder, NULL);
    }

    if (ret < 0) {
        ml_camera_set_err(cam, "avcodec_open2 failed");
        return -1;
    }

    fprintf(stderr, "[ffsdl] decoder=%s mode=%s stream=%dx%d\n",
            cam->mpp_decoder ? "mpp_direct" : cam->decoder->name,
            cam->hw_decoder ? "hardware" : "software",
            cam->codec_ctx->width, cam->codec_ctx->height);

    cam->dec_frame = ml_ffmpeg_frame_alloc();
    cam->rgb_frame = ml_ffmpeg_frame_alloc();
    if (!cam->dec_frame || !cam->rgb_frame)
        return -1;

    /* BGR888 output buffer (for swscale fallback) */
    cam->rgb_buf_size = av_image_get_buffer_size(
        AV_PIX_FMT_BGR24,
        cam->codec_ctx->width,
        cam->codec_ctx->height, 1);
    cam->rgb_buf = ml_ffmpeg_alloc(cam->rgb_buf_size);
    if (!cam->rgb_buf)
        return -1;

    av_image_fill_arrays(cam->rgb_frame->data, cam->rgb_frame->linesize,
                         cam->rgb_buf, AV_PIX_FMT_BGR24,
                         cam->codec_ctx->width, cam->codec_ctx->height, 1);

    /* RKMPP commonly outputs DRM_PRIME frames.  Do not pass that opaque
     * format to swscale during open: some FFmpeg/RKMPP combinations crash
     * there.  Create swscale only after transferring a frame to system RAM. */
    if (!cam->hw_decoder) {
        cam->sws = sws_getContext(
            cam->codec_ctx->width, cam->codec_ctx->height,
            cam->codec_ctx->pix_fmt,
            cam->codec_ctx->width, cam->codec_ctx->height,
            AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
        if (!cam->sws)
            return -1;
    }

    /* try RGA for hardware NV12→BGR when using h264_rkmpp */
    if (cam->hw_decoder && ml_rga_available()) {
        cam->rga = ml_rga_create(cam->pool);
        if (!cam->rga) {
            /* RGA create failed — will use swscale fallback */
        }
    }

    /* RTSP must continuously drain the demuxer.  Sleeping before
     * av_read_frame() lets the RTP jitter buffer overflow on sources whose
     * native FPS is higher than the configured business FPS, causing missed
     * packets and H.264 macroblock corruption.  Downstream stages may drop or
     * sample frames; the network reader should not throttle. */
    cam->min_frame_us = 0;

    cam->reconnect_delay = 1;
    cam->sw_frame = ml_ffmpeg_frame_alloc();
    if (!cam->sw_frame)
        return -1;
    return 0;
}

static void rtsp_close(ml_camera_t *cam)
{
    if (cam->rga)         { ml_rga_destroy(cam->rga); cam->rga = NULL; }
    ml_mpp_decoder_destroy(&cam->mpp_decoder);
    ml_ffmpeg_free(cam->rga_cvt_buf);
    cam->rga_cvt_buf  = NULL;
    cam->rga_cvt_size = 0;

    if (cam->sws)         { sws_freeContext(cam->sws); cam->sws = NULL; }
    if (cam->rgb_buf)     { ml_ffmpeg_free(cam->rgb_buf); cam->rgb_buf = NULL; }
    if (cam->rgb_frame)   { ml_ffmpeg_frame_free(&cam->rgb_frame); }
    if (cam->dec_frame)   { ml_ffmpeg_frame_free(&cam->dec_frame); }
    if (cam->sw_frame)    { ml_ffmpeg_frame_free(&cam->sw_frame); }
    if (cam->codec_ctx)   { avcodec_free_context(&cam->codec_ctx); }
    if (cam->fmt_ctx)     { avformat_close_input(&cam->fmt_ctx); }
}

/* ── ops vtable ────────────────────────────────────────────────────────── */

const ml_camera_ops_t ml_camera_rtsp_ops = {
    .destroy       = ml_camera_rtsp_destroy,
    .start         = ml_camera_rtsp_start,
    .stop          = ml_camera_rtsp_stop,
    .read          = ml_camera_rtsp_read,
    .frame_release = ml_camera_rtsp_frame_release,
    .set_callback  = ml_camera_rtsp_set_callback,
    .get_width     = ml_camera_rtsp_get_width,
    .get_height    = ml_camera_rtsp_get_height,
    .get_fps       = ml_camera_rtsp_get_fps,
    .get_state     = ml_camera_rtsp_get_state,
    .error         = ml_camera_rtsp_error,
};

/* ── lifecycle ────────────────────────────────────────────────────────── */

ml_camera_t *
ml_camera_rtsp_create(const ml_camera_cfg_t *cfg, ml_pool_t *pool)
{
    ml_camera_t *cam;

    if (!cfg || !cfg->device)
        return NULL;
    if (!pool)
        return NULL;

    cam = pool->f_alloc(pool, sizeof(*cam));
    if (!cam)
        return NULL;
    memset(cam, 0, sizeof(*cam));

    cam->backend = ML_CAMERA_BACKEND_RTSP;
    cam->cfg     = *cfg;
    cam->pool    = pool;
    cam->state   = ML_CAMERA_STATE_STOPPED;
    cam->ops     = &ml_camera_rtsp_ops;

    if (rtsp_open(cam) < 0) {
        rtsp_close(cam);
        pool->f_free(pool, cam);
        return NULL;
    }

    return cam;
}

void
ml_camera_rtsp_destroy(ml_camera_t *cam)
{
    if (!cam)
        return;

    ml_camera_rtsp_stop(cam);
    rtsp_close(cam);
    if (cam->pool) cam->pool->f_free(cam->pool, cam);
}

/* ── start / stop ─────────────────────────────────────────────────────── */

int ml_camera_rtsp_start(ml_camera_t *cam)
{
    if (!cam)
        return -1;
    cam->state = ML_CAMERA_STATE_RUNNING;

    if (cam->callback) {
        cam->thread_running = 1;
        if (pthread_create(&cam->thread, NULL, rtsp_thread, cam) != 0) {
            cam->thread_running = 0;
        }
    }
    return 0;
}

int ml_camera_rtsp_stop(ml_camera_t *cam)
{
    if (!cam)
        return -1;

    if (cam->thread_running) {
        cam->thread_running = 0;
        cam->interrupt_requested = 1;
        pthread_join(cam->thread, NULL);
        cam->interrupt_requested = 0;
    }

    cam->state = ML_CAMERA_STATE_STOPPED;
    return 0;
}

/* ── synchronous read ─────────────────────────────────────────────────── */

static int rtsp_read_internal(ml_camera_t *cam, ml_frame_t *frame)
{
    AVPacket *pkt = NULL;
    int ret, got_frame = 0;
    uint64_t frame_ts;

    /* wall-clock fallback when decoder PTS is unavailable */
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        frame_ts = (uint64_t)(now.tv_sec * 1000000LL + now.tv_nsec / 1000);
    }

    /* Bounded decode loop (see 03-clib-audit.md C3).
     * Without a max_tries cap, avcodec_receive_frame returning
     * persistent EAGAIN (decoder buffer not full) or the upstream
     * sending a long run of non-video packets would spin forever,
     * blocking rtsp_thread and stalling stop()'s join. */
    int max_tries = 500;  /* generous: ~16s at 30fps, longer if av_read_frame blocks */
    while (!got_frame) {
        if (--max_tries <= 0) {
            /* pkt is either NULL (first iter) or already freed by prior
             * iter's ml_ffmpeg_packet_free — do not double-free. */
            return -1;
        }

        pkt = ml_ffmpeg_packet_alloc();
        if (!pkt)
            return -1;

        ret = av_read_frame(cam->fmt_ctx, pkt);
        if (ret < 0) {
            ml_ffmpeg_packet_free(&pkt);
            return -1;
        }

        if (pkt->stream_index == cam->video_idx) {
#ifdef __linux__
            if (cam->mpp_decoder) {
                uint8_t *nv12 = NULL;
                int width = 0;
                int height = 0;
                int stride = 0;
                int vstride = 0;

                ret = ml_mpp_decoder_decode(cam->mpp_decoder,
                                            pkt->data,
                                            (size_t)pkt->size,
                                            pkt->pts,
                                            &nv12,
                                            &width,
                                            &height,
                                            &stride,
                                            &vstride);
                (void)vstride;
                if (ret < 0) {
                    ml_ffmpeg_packet_free(&pkt);
                    return -1;
                }
                if (ret > 0) {
                    av_frame_unref(cam->dec_frame);
                    cam->dec_frame->format = AV_PIX_FMT_NV12;
                    cam->dec_frame->width = width;
                    cam->dec_frame->height = height;
                    av_image_fill_arrays(cam->dec_frame->data,
                                         cam->dec_frame->linesize,
                                         nv12,
                                         AV_PIX_FMT_NV12,
                                         width,
                                         height,
                                         1);
                    cam->dec_frame->linesize[0] = stride;
                    cam->dec_frame->linesize[1] = stride;
                    cam->dec_frame->pts = pkt->pts;
                    got_frame = 1;
                }
            } else {
#endif
#if LIBAVCODEC_VERSION_MAJOR < 59
            /* FFmpeg 4.3's RKMPP decoder uses the legacy decode callback and
             * internally drains its packet queue from receive_frame(). */
            ret = avcodec_decode_video2(cam->codec_ctx, cam->dec_frame,
                                        &got_frame, pkt);
            if (ret < 0) {
                ml_ffmpeg_packet_free(&pkt);
                return -1;
            }
#else
            ret = avcodec_send_packet(cam->codec_ctx, pkt);
            if (ret == 0) {
                ret = avcodec_receive_frame(cam->codec_ctx, cam->dec_frame);
                if (ret == 0) {
                    got_frame = 1;
                    /* AV_EF_IGNORE_ERR keeps decode errors from failing the
                     * stream; surface the concealment tail so long-term
                     * source degradation stays observable in the field. */
                    if (cam->dec_frame->decode_error_flags != 0) {
                        cam->corrupt_frames++;
                        if ((cam->corrupt_frames & 63) == 1) {
                            char msg[96];
                            int n = snprintf(msg, sizeof(msg),
                                "[ml_camera_rtsp] corrupt frames decoded: %llu\n",
                                (unsigned long long)cam->corrupt_frames);
                            if (n > 0) fputs(msg, stderr);
                        }
                    }
                } else if (ret != AVERROR(EAGAIN)) {
                    /* hard decoder error (not EAGAIN) — bail out */
                    ml_ffmpeg_packet_free(&pkt);
                    return -1;
                }
                /* EAGAIN: decoder needs more packets, loop */
            } else if (ret == AVERROR(EAGAIN)) {
                /* Hardware decoders may report a full input queue.  Drain
                 * output and retry this same packet; dropping it can starve
                 * RKMPP indefinitely, especially while waiting for an IDR. */
                int retry = 100;
                while (!got_frame && retry-- > 0) {
                    int out_ret = avcodec_receive_frame(cam->codec_ctx,
                                                        cam->dec_frame);
                    if (out_ret == 0) {
                        got_frame = 1;
                        break;
                    }
                    if (out_ret != AVERROR(EAGAIN)) {
                        ml_ffmpeg_packet_free(&pkt);
                        return -1;
                    }
                    usleep(1000);
                    ret = avcodec_send_packet(cam->codec_ctx, pkt);
                    if (ret == 0)
                        continue;
                    if (ret != AVERROR(EAGAIN)) {
                        ml_ffmpeg_packet_free(&pkt);
                        return -1;
                    }
                }
            } else {
                /* send_packet hard error (not EAGAIN) — bail out */
                ml_ffmpeg_packet_free(&pkt);
                return -1;
            }
            /* send_packet EAGAIN: decoder buffer full, loop to receive */
#endif
#ifdef __linux__
            }
#endif
        }
        ml_ffmpeg_packet_free(&pkt);
    }

    /* convert decoder PTS (stream timebase) → microseconds.
     * Only trust PTS when the converted value is within a sane range;
     * RTSP/RTP timestamps can be random 32-bit rollover values that
     * produce nonsense after timebase rescaling. */
    if (cam->dec_frame->pts != AV_NOPTS_VALUE) {
        AVRational tb = cam->fmt_ctx->streams[cam->video_idx]->time_base;
        int64_t pts_us = av_rescale_q(cam->dec_frame->pts, tb, AV_TIME_BASE_Q);
        if (pts_us > 0 && pts_us < (INT64_MAX / 2))
            frame_ts = (uint64_t)pts_us;
    }

    /* Hardware decoders can return opaque DRM_PRIME frames.  Transfer them
     * to ordinary system memory before RGA/swscale touches the planes. */
    if (cam->hw_decoder && cam->dec_frame->format == AV_PIX_FMT_DRM_PRIME) {
        if (av_hwframe_transfer_data(cam->sw_frame, cam->dec_frame, 0) < 0)
            return -1;
    } else if (cam->hw_decoder) {
        av_frame_unref(cam->sw_frame);
        if (av_frame_ref(cam->sw_frame, cam->dec_frame) < 0)
            return -1;
    }

    AVFrame *convert_frame = cam->hw_decoder ? cam->sw_frame : cam->dec_frame;

    if (cam->hw_decoder && !cam->sws) {
        cam->sws = sws_getContext(
            cam->codec_ctx->width, cam->codec_ctx->height,
            convert_frame->format,
            cam->codec_ctx->width, cam->codec_ctx->height,
            AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
        if (!cam->sws)
            return -1;
    }

    /* color conversion: RGA (hw) preferred, swscale fallback */
    if (cam->rga && cam->dec_frame->format != AV_PIX_FMT_DRM_PRIME) {
        if (rga_nv12_to_bgr(cam) == 0) {
            ml_frame_from_mem(
                frame,
                cam->rga_cvt_buf, cam->rga_cvt_size,
                cam->codec_ctx->width, cam->codec_ctx->height,
                ML_PIXFMT_BGR888,
                frame_ts, 0);
            /* Copy frame data to per-read buffer (UAF safety, clib M4).
             * Internal rga_cvt_buf is reused each frame; without a copy,
             * any caller holding a frame reference after the next read()
             * would read overwritten data. */
            {
                void *copy = ml_ffmpeg_alloc(frame->size[0]);
                if (!copy)
                    return -1;
                memcpy(copy, frame->data[0], frame->size[0]);
                frame->data[0] = copy;
                frame->release = rtsp_frame_release;
            }
            return 0;
        }
        /* RGA failed — fall through to swscale */
    }

    /* swscale fallback */
    sws_scale(cam->sws,
              (const uint8_t * const *)convert_frame->data,
              convert_frame->linesize, 0,
              cam->codec_ctx->height,
              cam->rgb_frame->data, cam->rgb_frame->linesize);

    ml_frame_from_mem(
        frame,
        cam->rgb_buf, cam->rgb_buf_size,
        cam->codec_ctx->width, cam->codec_ctx->height,
        ML_PIXFMT_BGR888,
        frame_ts, 0);

    /* Same UAF-safety copy for swscale path */
    {
        void *copy = ml_ffmpeg_alloc(frame->size[0]);
        if (!copy)
            return -1;
        memcpy(copy, frame->data[0], frame->size[0]);
        frame->data[0] = copy;
        frame->release = rtsp_frame_release;
    }

    return 0;
}

int
ml_camera_rtsp_read(ml_camera_t *cam, ml_frame_t *frame, int timeout_ms)
{
    int ret;

    if (!cam || !frame)
        return -1;
    if (cam->state != ML_CAMERA_STATE_RUNNING)
        return -1;

    /* set interrupt deadline for blocking FFmpeg I/O */
    if (timeout_ms > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        cam->deadline_us = now.tv_sec * 1000000LL +
                           now.tv_nsec / 1000 + timeout_ms * 1000LL;
    } else {
        cam->deadline_us = 0; /* no timeout */
    }

    /* FPS limiting: sleep to maintain target frame rate */
    if (cam->min_frame_us > 0 && cam->last_frame_time_us > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t now_us = now.tv_sec * 1000000LL + now.tv_nsec / 1000;
        int64_t elapsed = now_us - (int64_t)cam->last_frame_time_us;
        if (elapsed < cam->min_frame_us) {
            usleep((useconds_t)(cam->min_frame_us - elapsed));
        }
    }

    ret = rtsp_read_internal(cam, frame);
    if (ret < 0) {
        /* auto-reconnect */
        rtsp_close(cam);
        cam->state = ML_CAMERA_STATE_ERROR;

        /* interruptible sleep — check thread_running every 100 ms
         * so that stop() can join the thread without blocking for up
         * to 30 s (worst-case exponential backoff). */
        for (int d = cam->reconnect_delay * 10;
             d > 0 && cam->thread_running; d--)
            usleep(100000);
        if (!cam->thread_running)
            return -1;

        if (cam->reconnect_delay < 30)
            cam->reconnect_delay *= 2;

        if (rtsp_open(cam) == 0) {
            cam->state = ML_CAMERA_STATE_RUNNING;
            cam->reconnect_delay = 1;
        } else {
            /* rtsp_open made partial allocations before failing;
             * must clean them up so they don't accumulate on retries. */
            rtsp_close(cam);
        }
        return -1;
    }

    /* record wall-clock delivery time for FPS throttle */
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        cam->last_frame_time_us = (uint64_t)(now.tv_sec * 1000000LL +
                                             now.tv_nsec / 1000);
    }
    return 0;
}

void
ml_camera_rtsp_frame_release(ml_camera_t *cam, ml_frame_t *frame)
{
    (void)cam;
    ml_frame_unref(frame);
}

/* ── async callback ───────────────────────────────────────────────────── */

static void *rtsp_thread(void *arg)
{
    ml_camera_t *cam = (ml_camera_t *)arg;
    ml_frame_t frame;
    ml_frame_init(&frame);

    while (cam->thread_running) {
        /* use finite timeout so av_read_frame can be interrupted
         * by the interrupt callback when stop() sets interrupt_requested */
        if (ml_camera_rtsp_read(cam, &frame, 5000) < 0)
            continue;

        if (cam->callback) {
            if (cam->callback(&frame, cam->cb_data) < 0) {
                ml_camera_rtsp_frame_release(cam, &frame);
                break;
            }
        }

        ml_camera_rtsp_frame_release(cam, &frame);
    }

    return NULL;
}

int
ml_camera_rtsp_set_callback(ml_camera_t *cam, ml_camera_frame_cb cb, void *data)
{
    if (!cam)
        return -1;
    if (cam->state != ML_CAMERA_STATE_STOPPED)
        return -1;

    cam->callback = cb;
    cam->cb_data  = data;
    return 0;
}

/* ── query ────────────────────────────────────────────────────────────── */

int ml_camera_rtsp_get_width(ml_camera_t *cam)
{
    return (cam && cam->codec_ctx) ? cam->codec_ctx->width : -1;
}

int ml_camera_rtsp_get_height(ml_camera_t *cam)
{
    return (cam && cam->codec_ctx) ? cam->codec_ctx->height : -1;
}

int ml_camera_rtsp_get_fps(ml_camera_t *cam)
{
    return cam ? cam->cfg.fps_num / (cam->cfg.fps_den ? cam->cfg.fps_den : 1) : -1;
}

int ml_camera_rtsp_get_state(ml_camera_t *cam)
{
    return cam ? cam->state : -1;
}

const char *
ml_camera_rtsp_error(ml_camera_t *cam)
{
    if (!cam || !cam->err_msg[0])
        return NULL;
    return cam->err_msg;
}
