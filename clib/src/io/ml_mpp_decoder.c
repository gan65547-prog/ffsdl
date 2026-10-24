/*
 * SPDX-License-Identifier: MIT
 * Direct Rockchip MPP decoder implementation.
 */
#include "io/ml_mpp_decoder.h"
#include "memory/ml_pool.h"

#ifdef __linux__
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

struct ml_mpp_decoder_s {
    MppCtx ctx;
    MppApi *mpi;
    MppPacket packet;
    MppCodingType coding;
    uint8_t *output;
    size_t output_size;
    int info_ready;
    MppBuffer held_buffer;
    MppBufferGroup output_group;
    MppBuffer output_buffer;
    int output_read_pending;
    ml_pool_t *pool;
};

static int mpp_copy_nv12(ml_mpp_decoder_t *decoder,
                         MppFrame frame,
                         uint8_t **nv12,
                         int *width,
                         int *height,
                         int *stride,
                         int *vstride)
{
    MppBuffer buffer;
    uint8_t *source;
    int frame_width;
    int frame_height;
    int frame_stride;
    int frame_vstride;
    size_t required;
    int row;

    buffer = mpp_frame_get_buffer(frame);
    if (!buffer) {
        return -1;
    }

    frame_width = (int)mpp_frame_get_width(frame);
    frame_height = (int)mpp_frame_get_height(frame);
    frame_stride = (int)mpp_frame_get_hor_stride(frame);
    frame_vstride = (int)mpp_frame_get_ver_stride(frame);
    if (frame_width <= 0 || frame_height <= 0 ||
        frame_stride < frame_width || frame_vstride < frame_height) {
        return -1;
    }

    required = (size_t)frame_width * (size_t)frame_height * 3U / 2U;
    if (decoder->output_size < required) {
        uint8_t *new_output = decoder->pool->f_alloc(decoder->pool, required);
        if (!new_output) {
            return -1;
        }
        if (decoder->output)
            decoder->pool->f_free(decoder->pool, decoder->output);
        decoder->output = new_output;
        decoder->output_size = required;
    }

    source = (uint8_t *)mpp_buffer_get_ptr(buffer);
    if (!source) {
        return -1;
    }

    for (row = 0; row < frame_height; ++row) {
        memcpy(decoder->output + (size_t)row * (size_t)frame_width,
               source + (size_t)row * (size_t)frame_stride,
               (size_t)frame_width);
    }
    source += (size_t)frame_stride * (size_t)frame_vstride;
    for (row = 0; row < frame_height / 2; ++row) {
        memcpy(decoder->output + (size_t)frame_width * (size_t)frame_height +
                   (size_t)row * (size_t)frame_width,
               source + (size_t)row * (size_t)frame_stride,
               (size_t)frame_width);
    }

    *nv12 = decoder->output;
    *width = frame_width;
    *height = frame_height;
    *stride = frame_width;
    *vstride = frame_height;
    return 0;
}

ml_mpp_decoder_t *ml_mpp_decoder_create(int codec_id, ml_pool_t *pool)
{
    ml_mpp_decoder_t *decoder;
    MPP_RET ret;

    if (!pool)
        return NULL;
    decoder = pool->f_alloc(pool, sizeof(*decoder));
    if (!decoder) {
        return NULL;
    }
    memset(decoder, 0, sizeof(*decoder));
    decoder->pool = pool;
    decoder->coding = codec_id == 173 ? MPP_VIDEO_CodingHEVC
                                      : MPP_VIDEO_CodingAVC;
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        pool->f_free(pool, decoder);
        return NULL;
    }
    /* Sync (no-thread) mode with the easy decode() call hands out B-frame
     * slots before the hardware writes them, which shows up as solid-green
     * frames.  Threaded mode + put/get is the canonical MPP usage and only
     * returns buffers the decoder has finished writing.  FFSDL_MPP_SYNC=1
     * restores the legacy sync behaviour for A/B testing. */
    if (getenv("FFSDL_MPP_SYNC")) {
        ret = decoder->mpi->control(decoder->ctx, MPP_SET_DISABLE_THREAD, NULL);
        if (ret != MPP_OK) {
            mpp_destroy(decoder->ctx);
            pool->f_free(pool, decoder);
            return NULL;
        }
    }
    /* MPP_DEC_SET_PARSER_SPLIT_MODE is deliberately NOT set: on this
     * firmware the split-mode parser emits only every other packet's frame
     * (50% loss) and correlates with never-written green output buffers.
     * The RTSP demuxer already delivers complete access units per packet. */
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, decoder->coding);
    if (ret != MPP_OK) {
        mpp_destroy(decoder->ctx);
        pool->f_free(pool, decoder);
        return NULL;
    }
    ret = mpp_packet_init(&decoder->packet, NULL, 0);
    if (ret != MPP_OK) {
        mpp_destroy(decoder->ctx);
        pool->f_free(pool, decoder);
        return NULL;
    }
    return decoder;
}

void ml_mpp_decoder_destroy(ml_mpp_decoder_t **decoder_ptr)
{
    ml_mpp_decoder_t *decoder;

    if (!decoder_ptr || !*decoder_ptr) {
        return;
    }
    decoder = *decoder_ptr;
    if (decoder->ctx) {
        if (decoder->held_buffer) mpp_buffer_put(decoder->held_buffer);
        if (decoder->output_buffer) mpp_buffer_put(decoder->output_buffer);
        if (decoder->output_group) mpp_buffer_group_put(decoder->output_group);
        mpp_packet_deinit(&decoder->packet);
        mpp_destroy(decoder->ctx);
    }
    if (decoder->output) decoder->pool->f_free(decoder->pool, decoder->output);
    decoder->pool->f_free(decoder->pool, decoder);
    *decoder_ptr = NULL;
}

int ml_mpp_decoder_acquire_output_dmabuf(ml_mpp_decoder_t *decoder,
                                         size_t size,
                                         ml_mpp_decoded_frame_t *out)
{
    MPP_RET ret;

    if (!decoder || !out || size == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    if (decoder->output_buffer && mpp_buffer_get_size(decoder->output_buffer) < size) {
        mpp_buffer_put(decoder->output_buffer);
        decoder->output_buffer = NULL;
    }
    if (!decoder->output_group) {
        ret = mpp_buffer_group_get_internal(&decoder->output_group,
                                            MPP_BUFFER_TYPE_DMA_HEAP |
                                            MPP_BUFFER_FLAGS_CONTIG);
        if (ret != MPP_OK)
            return -1;
    }
    if (!decoder->output_buffer) {
        ret = mpp_buffer_get(decoder->output_group, &decoder->output_buffer, size);
        if (ret != MPP_OK)
            return -1;
    }

    out->fd = mpp_buffer_get_fd(decoder->output_buffer);
    out->data = mpp_buffer_get_ptr(decoder->output_buffer);
    out->size = mpp_buffer_get_size(decoder->output_buffer);
    if (out->fd < 0 || !out->data || out->size < size)
        return -1;
    return 0;
}

int ml_mpp_decoder_decode(ml_mpp_decoder_t *decoder,
                          const uint8_t *data,
                          size_t size,
                          int64_t pts,
                          uint8_t **nv12,
                          int *width,
                          int *height,
                          int *stride,
                          int *vstride)
{
    MppFrame frame = NULL;
    MPP_RET ret;
    int result = 0;

    if (!decoder || !data || size == 0 || !nv12 || !width || !height ||
        !stride || !vstride) {
        return -1;
    }
    mpp_packet_set_data(decoder->packet, (void *)data);
    mpp_packet_set_size(decoder->packet, size);
    mpp_packet_set_pos(decoder->packet, (void *)data);
    mpp_packet_set_length(decoder->packet, size);
    mpp_packet_set_pts(decoder->packet, pts);
    ret = decoder->mpi->decode(decoder->ctx, decoder->packet, &frame);
    if (ret != MPP_OK || !frame) {
        return 0;
    }

    if (mpp_frame_get_info_change(frame)) {
        ret = decoder->mpi->control(decoder->ctx,
                                    MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if (ret != MPP_OK) {
            result = -1;
        } else {
            decoder->info_ready = 1;
        }
    } else if (!mpp_frame_get_discard(frame) &&
               mpp_frame_get_buffer(frame)) {
        result = mpp_copy_nv12(decoder, frame, nv12, width, height,
                                stride, vstride);
        if (result == 0) {
            result = 1;
        }
    }
    mpp_frame_deinit(&frame);
    return result;
}

int ml_mpp_decoder_output_sync(ml_mpp_decoder_t *decoder, int read_begin)
{
    if (!decoder)
        return -1;
    if (read_begin) {
        if (!decoder->output_buffer ||
            mpp_buffer_sync_ro_begin(decoder->output_buffer) != MPP_OK)
            return -1;
        decoder->output_read_pending = 1;
        return 0;
    }
    if (!decoder->output_read_pending)
        return 0;
    decoder->output_read_pending = 0;
    if (!decoder->output_buffer ||
        mpp_buffer_sync_ro_end(decoder->output_buffer) != MPP_OK)
        return -1;
    return 0;
}

int ml_mpp_decoder_decode_dmabuf(ml_mpp_decoder_t *decoder,
                                 const uint8_t *data, size_t size, int64_t pts,
                                 ml_mpp_decoded_frame_t *out)
{
    MppFrame frame = NULL;
    MppBuffer buffer;
    MPP_RET ret;

    if (!decoder || !data || !size || !out)
        return -1;
    /* The previous frame's CPU read (display/dump) is finished by now in the
     * single-threaded read loop; close its read sync before RGA writes again. */
    ml_mpp_decoder_output_sync(decoder, 0);
    if (decoder->held_buffer) {
        mpp_buffer_put(decoder->held_buffer);
        decoder->held_buffer = NULL;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    mpp_packet_set_data(decoder->packet, (void *)data);
    mpp_packet_set_size(decoder->packet, size);
    mpp_packet_set_pos(decoder->packet, (void *)data);
    mpp_packet_set_length(decoder->packet, size);
    mpp_packet_set_pts(decoder->packet, pts);
    /* put/get separation: get_frame only yields buffers the hardware has
     * finished writing.  put_packet stays busy (MPP_ERR_BUFFER_FULL) while
     * the input queue holds undecodable pre-IDR packets or the output
     * queue is congested.  Drain one output item per retry and back off
     * briefly; keep trying the SAME packet so the SPS/PPS/IDR that unblocks
     * the stream is never dropped. */
    ret = decoder->mpi->decode_put_packet(decoder->ctx, decoder->packet);
    {
        int retry = 250;
        while (ret != MPP_OK && retry-- > 0) {
            MppFrame drain = NULL;
            if (decoder->mpi->decode_get_frame(decoder->ctx, &drain) ==
                MPP_OK && drain) {
                if (mpp_frame_get_info_change(drain))
                    decoder->mpi->control(decoder->ctx,
                                          MPP_DEC_SET_INFO_CHANGE_READY,
                                          NULL);
                mpp_frame_deinit(&drain);
            }
            usleep(2000);
            ret = decoder->mpi->decode_put_packet(decoder->ctx,
                                                  decoder->packet);
        }
        if (ret != MPP_OK) {
            static unsigned busy_log;
            if ((busy_log++ & 63) == 0)
                fprintf(stderr, "[mpp] put_packet busy ret=%d\n", ret);
            return 0;
        }
    }
    frame = NULL;
    ret = decoder->mpi->decode_get_frame(decoder->ctx, &frame);
    if (ret != MPP_OK || !frame) {
        static unsigned no_frame_log;
        if (no_frame_log++ < 8)
            fprintf(stderr, "[mpp] get_frame ret=%d frame=%p\n", ret,
                    (void *)frame);
        return 0;
    }
    if (mpp_frame_get_info_change(frame)) {
        static unsigned info_log;
        if (info_log++ < 4)
            fprintf(stderr, "[mpp] info_change width=%d height=%d\n",
                    mpp_frame_get_width(frame), mpp_frame_get_height(frame));
        ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        mpp_frame_deinit(&frame);
        return ret == MPP_OK ? 0 : -1;
    }
    buffer = mpp_frame_get_buffer(frame);
    /* Discarded frames are normal while an RTSP subscriber waits for the
     * first IDR; they are not a decoder/RGA failure. */
    /* The Rockchip ABI exposes discard as a flag word; value 0x10 is a
     * normal view/metadata flag on this board, not a dropped frame. */
    if (mpp_frame_get_discard(frame) == 1 || !buffer) {
        static unsigned discard_log;
        if (discard_log++ < 8)
            fprintf(stderr, "[mpp] discard=%u viewid=%u mode=%u errinfo=%u pts=%lld poc=%u buffer=%p\n",
                    (unsigned)mpp_frame_get_discard(frame),
                    (unsigned)mpp_frame_get_viewid(frame),
                    (unsigned)mpp_frame_get_mode(frame),
                    (unsigned)mpp_frame_get_errinfo(frame),
                    (long long)mpp_frame_get_pts(frame),
                    (unsigned)mpp_frame_get_poc(frame), (void *)buffer);
        mpp_frame_deinit(&frame);
        return 0;
    }
    if (mpp_frame_get_fmt(frame) != MPP_FMT_YUV420SP) {
        fprintf(stderr, "[mpp] unsupported output format %#x\n",
                (unsigned)mpp_frame_get_fmt(frame));
        mpp_frame_deinit(&frame);
        return -1;
    }
    out->fd = mpp_buffer_get_fd(buffer);
    out->data = mpp_buffer_get_ptr(buffer);
    out->size = mpp_buffer_get_size(buffer);
    out->width = mpp_frame_get_width(frame);
    out->height = mpp_frame_get_height(frame);
    out->hor_stride = mpp_frame_get_hor_stride(frame);
    out->ver_stride = mpp_frame_get_ver_stride(frame);
    out->format = mpp_frame_get_fmt(frame);
    out->pts = mpp_frame_get_pts(frame);
    if (mpp_buffer_inc_ref(buffer) != MPP_OK) {
        mpp_frame_deinit(&frame);
        return -1;
    }
    decoder->held_buffer = buffer;
    mpp_frame_deinit(&frame);
    if (out->fd < 0 || !out->data || out->hor_stride < out->width ||
        out->ver_stride < out->height || out->size < (size_t)out->hor_stride * out->ver_stride * 3 / 2) {
        fprintf(stderr, "[mpp] invalid DMA-BUF fd=%d data=%p %dx%d stride=%dx%d size=%zu\n",
                out->fd, out->data, out->width, out->height,
                out->hor_stride, out->ver_stride, out->size);
        return -1;
    }
    return 1;
}

#else

struct ml_mpp_decoder_s {
    int unused;
};

ml_mpp_decoder_t *ml_mpp_decoder_create(int codec_id, ml_pool_t *pool)
{
    (void)codec_id;
    (void)pool;
    return NULL;
}

void ml_mpp_decoder_destroy(ml_mpp_decoder_t **decoder)
{
    (void)decoder;
}

int ml_mpp_decoder_decode(ml_mpp_decoder_t *decoder,
                          const uint8_t *data,
                          size_t size,
                          int64_t pts,
                          uint8_t **nv12,
                          int *width,
                          int *height,
                          int *stride,
                          int *vstride)
{
    (void)decoder;
    (void)data;
    (void)size;
    (void)pts;
    (void)nv12;
    (void)width;
    (void)height;
    (void)stride;
    (void)vstride;
    return -1;
}

int ml_mpp_decoder_decode_dmabuf(ml_mpp_decoder_t *decoder,
                                 const uint8_t *data, size_t size, int64_t pts,
                                 ml_mpp_decoded_frame_t *out)
{
    (void)decoder; (void)data; (void)size; (void)pts; (void)out;
    return -1;
}

int ml_mpp_decoder_acquire_output_dmabuf(ml_mpp_decoder_t *decoder,
                                         size_t size,
                                         ml_mpp_decoded_frame_t *out)
{
    (void)decoder; (void)size; (void)out;
    return -1;
}

int ml_mpp_decoder_output_sync(ml_mpp_decoder_t *decoder, int read_begin)
{
    (void)decoder; (void)read_begin;
    return -1;
}

#endif
