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

ml_mpp_decoder_t *ml_mpp_decoder_create(int codec_id);
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

#endif
