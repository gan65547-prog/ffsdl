/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_frame deep-copy implementation (needs pool access).
 * Everything else in ml_frame.h is header-only inline.
 */

#include "memory/ml_frame.h"
#include "memory/ml_pool.h"

int
ml_frame_deep_copy(ml_frame_t *dst, const ml_frame_t *src, ml_pool_t *pool)
{
    unsigned char *buf;
    size_t         total;

    if (!dst || !src || !pool)
        return -1;

    total = ml_frame_total_size(src);
    if (total == 0)
        return -1;

    buf = pool->f_alloc(pool, total);
    if (!buf)
        return -1;

    *dst = *src;
    dst->refcount = 1;
    dst->flags   &= ~ML_FRAME_FLAG_OWN_FD;
    dst->owner    = NULL;
    dst->release  = NULL;

    memcpy(buf, src->data[0], src->size[0]);
    dst->data[0] = buf;
    dst->fd[0]   = -1;

    if (src->num_planes >= 2 && src->size[1] > 0) {
        buf += src->size[0];
        memcpy(buf, src->data[1], src->size[1]);
        dst->data[1] = buf;
        dst->fd[1]   = -1;
    }
    if (src->num_planes >= 3 && src->size[2] > 0) {
        buf += src->size[1];
        memcpy(buf, src->data[2], src->size[2]);
        dst->data[2] = buf;
        dst->fd[2]   = -1;
    }

    return 0;
}
