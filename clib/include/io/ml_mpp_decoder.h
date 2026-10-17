/*
 * SPDX-License-Identifier: MIT
 * Direct Rockchip MPP decoder used when the FFmpeg RKMPP wrapper is
 * unavailable or incompatible with the board MPP ABI.
 */
#ifndef __ML_MPP_DECODER_H__
#define __ML_MPP_DECODER_H__

#include <stddef.h>
#include <stdint.h>

typedef struct ml_mpp_decoder_s ml_mpp_decoder_t;
typedef struct ml_pool_s ml_pool_t;

typedef struct {
    int      fd;
    void    *data;
    size_t   size;
    int      width;
    int      height;
    int      hor_stride;
    int      ver_stride;
    int      format;
    int64_t  pts;
} ml_mpp_decoded_frame_t;

ml_mpp_decoder_t *ml_mpp_decoder_create(int codec_id, ml_pool_t *pool);
void ml_mpp_decoder_destroy(ml_mpp_decoder_t **decoder);
int ml_mpp_decoder_decode(ml_mpp_decoder_t *decoder,
                          const uint8_t *data,
                          size_t size,
                          int64_t pts,
                          uint8_t **nv12,
                          int *width,
                          int *height,
                          int *stride,
                          int *vstride);

/* Decode one access unit and expose the native MPP DMA-BUF metadata.
 * The returned mapping/fd is valid only until the next decode call. */
int ml_mpp_decoder_decode_dmabuf(ml_mpp_decoder_t *decoder,
                                 const uint8_t *data, size_t size, int64_t pts,
                                 ml_mpp_decoded_frame_t *out);

/* Acquire the decoder-owned DMA-BUF used as the RGA output surface.  It is
 * reused between calls and remains valid until decoder destruction. */
int ml_mpp_decoder_acquire_output_dmabuf(ml_mpp_decoder_t *decoder,
                                         size_t size,
                                         ml_mpp_decoded_frame_t *out);

/* Cache-sync the RGA output DMA-BUF for CPU access: read_begin=1 before the
 * CPU reads it, read_begin=0 when the CPU is done. */
int ml_mpp_decoder_output_sync(ml_mpp_decoder_t *decoder, int read_begin);

#endif
