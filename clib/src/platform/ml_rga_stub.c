/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_rga_stub.c — macOS / non-Rockchip stub.
 */

#include "platform/ml_rga.h"
#include "memory/ml_pool.h"
#include <stdlib.h>

struct ml_rga_s {
    ml_pool_t *pool;
    char       err_msg[256];
};

ml_rga_t *ml_rga_create(ml_pool_t *pool)
{
    if (!pool)
        return NULL;
    ml_rga_t *rga = pool->f_alloc(pool, sizeof(*rga));
    if (!rga)
        return NULL;
    memset(rga, 0, sizeof(*rga));
    rga->pool = pool;
    snprintf(rga->err_msg, sizeof(rga->err_msg),
             "RGA not available on this platform");
    /* return non-NULL so API doesn't crash, but all ops fail */
    return rga;
}

void ml_rga_destroy(ml_rga_t *rga) {
    if (rga && rga->pool) rga->pool->f_free(rga->pool, rga);
}

int ml_rga_resize(ml_rga_t *rga,
                  const void *s, int sw, int sh,
                  void *d, int dw, int dh, ml_rga_format_t f)
{ (void)rga; (void)s; (void)sw; (void)sh; (void)d; (void)dw; (void)dh; (void)f; return -1; }

int ml_rga_csc(ml_rga_t *rga,
               const void *s, int w, int h, ml_rga_format_t sf,
               void *d, ml_rga_format_t df)
{ (void)rga; (void)s; (void)w; (void)h; (void)sf; (void)d; (void)df; return -1; }

int ml_rga_execute(ml_rga_t *rga, const ml_rga_op_t *op)
{ (void)rga; (void)op; return -1; }

int ml_rga_nv12_to_bgr(ml_rga_t *rga,
                        int src_fd, const void *src_data,
                        int src_w, int src_h,
                        int src_hor_stride, int src_ver_stride,
                        void *dst, int dst_w, int dst_h)
{ (void)rga; (void)src_fd; (void)src_data; (void)src_w; (void)src_h;
  (void)src_hor_stride; (void)src_ver_stride; (void)dst; (void)dst_w; (void)dst_h; return -1; }

/* CPU fallback: pure software, no RGA hardware dependency.
 * Included in stub so non-Rockchip platforms can still link. */
int ml_rga_nv12_to_bgr_cpu(const uint8_t *y_plane, const uint8_t *uv_plane,
                            int src_w, int src_h,
                            int y_stride, int uv_stride,
                            uint8_t *dst, int dst_w, int dst_h)
{
    int x, y;
    if (!y_plane || !uv_plane || !dst)
        return -1;
    if (src_w < 2 || src_h < 2)
        return -1;
    if (dst_w <= 0 || dst_h <= 0)
        return -1;
    if (y_stride < src_w || uv_stride < src_w)
        return -1;

    for (y = 0; y < dst_h; y++) {
        float sy = (float)(y + 0.5f) * (float)src_h / (float)dst_h - 0.5f;
        int   sy0 = (int)sy;
        float sy_frac = sy - (float)sy0;
        if (sy0 < 0) { sy0 = 0; sy_frac = 0.0f; }
        if (sy0 >= src_h - 1) { sy0 = src_h - 2; sy_frac = 1.0f; }

        for (x = 0; x < dst_w; x++) {
            float sx = (float)(x + 0.5f) * (float)src_w / (float)dst_w - 0.5f;
            int   sx0 = (int)sx;
            float sx_frac = sx - (float)sx0;
            if (sx0 < 0) { sx0 = 0; sx_frac = 0.0f; }
            if (sx0 >= src_w - 1) { sx0 = src_w - 2; sx_frac = 1.0f; }

            float y00 = (float)y_plane[sy0 * y_stride + sx0];
            float y10 = (float)y_plane[sy0 * y_stride + sx0 + 1];
            float y01 = (float)y_plane[(sy0 + 1) * y_stride + sx0];
            float y11 = (float)y_plane[(sy0 + 1) * y_stride + sx0 + 1];
            float Y = y00 * (1.0f - sx_frac) * (1.0f - sy_frac)
                    + y10 * sx_frac * (1.0f - sy_frac)
                    + y01 * (1.0f - sx_frac) * sy_frac
                    + y11 * sx_frac * sy_frac;

            int uv_x = sx0 / 2;
            int uv_y = sy0 / 2;
            float U = (float)uv_plane[uv_y * uv_stride + uv_x * 2];
            float V = (float)uv_plane[uv_y * uv_stride + uv_x * 2 + 1];

            float R = Y + 1.402f   * (V - 128.0f);
            float G = Y - 0.34414f * (U - 128.0f) - 0.71414f * (V - 128.0f);
            float B = Y + 1.772f   * (U - 128.0f);

            int ir = (int)(R + 0.5f);
            int ig = (int)(G + 0.5f);
            int ib = (int)(B + 0.5f);
            uint8_t *p = dst + (y * dst_w + x) * 3;
            p[0] = (uint8_t)(ib < 0 ? 0 : (ib > 255 ? 255 : ib));
            p[1] = (uint8_t)(ig < 0 ? 0 : (ig > 255 ? 255 : ig));
            p[2] = (uint8_t)(ir < 0 ? 0 : (ir > 255 ? 255 : ir));
        }
    }
    return 0;
}

int ml_rga_nv16_to_bgr(ml_rga_t *rga,
                        int src_fd, const void *src_data,
                        int src_w, int src_h,
                        int src_hor_stride, int src_ver_stride,
                        void *dst, int dst_w, int dst_h)
{ (void)rga; (void)src_fd; (void)src_data; (void)src_w; (void)src_h;
  (void)src_hor_stride; (void)src_ver_stride; (void)dst; (void)dst_w; (void)dst_h; return -1; }

int ml_rga_execute_frame(ml_rga_t *rga,
                          const ml_frame_t *src, ml_frame_t *dst,
                          int dst_w, int dst_h)
{ (void)rga; (void)src; (void)dst; (void)dst_w; (void)dst_h; return -1; }

int ml_rga_rgb_to_nv12_cpu(const void *src, int w, int h,
                            ml_rga_format_t src_fmt,
                            uint8_t *y_plane, uint8_t *uv_plane,
                            int y_stride)
{ (void)src; (void)w; (void)h; (void)src_fmt; (void)y_plane; (void)uv_plane; (void)y_stride; return -1; }

const char *ml_rga_error(ml_rga_t *rga)
{
    if (!rga || !rga->err_msg[0])
        return NULL;
    return rga->err_msg;
}

int ml_rga_available(void) { return 0; }
