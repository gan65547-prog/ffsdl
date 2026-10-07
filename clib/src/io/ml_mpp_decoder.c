/*
 * SPDX-License-Identifier: MIT
 * Direct Rockchip MPP decoder implementation.
 */
#include "io/ml_mpp_decoder.h"

#ifdef __linux__
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>

#include <stdlib.h>
#include <string.h>

struct ml_mpp_decoder_s {
    MppCtx ctx;
    MppApi *mpi;
    MppPacket packet;
    MppCodingType coding;
    uint8_t *output;
    size_t output_size;
    int info_ready;
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
        uint8_t *new_output = realloc(decoder->output, required);
        if (!new_output) {
            return -1;
        }
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

ml_mpp_decoder_t *ml_mpp_decoder_create(int codec_id)
{
    ml_mpp_decoder_t *decoder;
    MPP_RET ret;
    RK_U32 split_mode = 1;

    decoder = calloc(1, sizeof(*decoder));
    if (!decoder) {
        return NULL;
    }
    decoder->coding = codec_id == 173 ? MPP_VIDEO_CodingHEVC
                                      : MPP_VIDEO_CodingAVC;
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        free(decoder);
        return NULL;
    }
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_DISABLE_THREAD, NULL);
    if (ret != MPP_OK) {
        mpp_destroy(decoder->ctx);
        free(decoder);
        return NULL;
    }
    /* RTSP demuxing supplies one or more NAL units per packet.  Enabling
     * parser split mode before init lets this MPP ABI safely consume packet
     * boundaries that do not coincide with complete frames. */
    ret = decoder->mpi->control(decoder->ctx,
                                MPP_DEC_SET_PARSER_SPLIT_MODE,
                                &split_mode);
    if (ret != MPP_OK) {
        mpp_destroy(decoder->ctx);
        free(decoder);
        return NULL;
    }
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, decoder->coding);
    if (ret != MPP_OK) {
        mpp_destroy(decoder->ctx);
        free(decoder);
        return NULL;
    }
    ret = mpp_packet_init(&decoder->packet, NULL, 0);
    if (ret != MPP_OK) {
        mpp_destroy(decoder->ctx);
        free(decoder);
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
        mpp_packet_deinit(&decoder->packet);
        mpp_destroy(decoder->ctx);
    }
    free(decoder->output);
    free(decoder);
    *decoder_ptr = NULL;
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

#else

struct ml_mpp_decoder_s {
    int unused;
};

ml_mpp_decoder_t *ml_mpp_decoder_create(int codec_id)
{
    (void)codec_id;
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

#endif
