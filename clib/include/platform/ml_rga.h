/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_rga — Rockchip RGA 2D accelerator wrapper.
 *
 * Provides zero-copy image resize, color-space conversion (CSC),
 * rotation, and flip via the hardware RGA engine.  On non-Rockchip
 * platforms, all operations return ML_ENOSYS.
 *
 * Lifecycle:
 *   create → execute* (or convenience wrappers) → destroy
 */

#ifndef __ML_RGA_H__
#define __ML_RGA_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── forward declarations ─────────────────────────────────────────────── */

typedef struct ml_pool_s ml_pool_t;
typedef struct ml_rga_s  ml_rga_t;
typedef struct ml_frame_s ml_frame_t;

/* ── enums ────────────────────────────────────────────────────────────── */

typedef enum {
    ML_RGA_FMT_BGR888 = 0,
    ML_RGA_FMT_RGB888,
    ML_RGA_FMT_NV12,     /* Y + UV two-plane 4:2:0 (MPP native) */
    ML_RGA_FMT_NV16,     /* Y + UV two-plane 4:2:2 (MPP 422)   */
    ML_RGA_FMT_NV21,     /* Y + VU two-plane                    */
    ML_RGA_FMT_RGBA8888,
    ML_RGA_FMT_BGRA8888,
} ml_rga_format_t;

typedef enum {
    ML_RGA_ROTATE_0   = 0,
    ML_RGA_ROTATE_90  = 1,
    ML_RGA_ROTATE_180 = 2,
    ML_RGA_ROTATE_270 = 3,
} ml_rga_rotate_t;

typedef enum {
    ML_RGA_FLIP_NONE       = 0,
    ML_RGA_FLIP_HORIZONTAL = 1,
    ML_RGA_FLIP_VERTICAL   = 2,
    ML_RGA_FLIP_BOTH       = 3,
} ml_rga_flip_t;

/* ── RGA operation descriptor ─────────────────────────────────────────── */

typedef struct {
    /* source */
    int             src_fd;       /* DMA-BUF fd, or -1 for user-space data */
    const void     *src_data;     /* user-space pointer if src_fd == -1   */
    size_t          src_size;     /* if >0, validated against min required
                                     bytes for format×width×height×stride  */
    int             src_width;
    int             src_height;
    int             src_stride;   /* virtual width in pixels (NOT bytes)  */
    ml_rga_format_t src_format;

    /* source ROI (region of interest) — 0 means full image */
    int             src_x, src_y;
    int             src_w, src_h; /* 0 = use src_width/height            */

    /* destination */
    int             dst_fd;       /* DMA-BUF fd, or -1                    */
    void           *dst_data;     /* user-space pointer if dst_fd == -1   */
    size_t          dst_size;     /* if >0, validated against min required
                                     bytes for format×width×height×stride  */
    int             dst_width;
    int             dst_height;
    int             dst_stride;   /* virtual width in pixels (NOT bytes)  */
    ml_rga_format_t dst_format;

    /* destination ROI */
    int             dst_x, dst_y;
    int             dst_w, dst_h; /* 0 = use dst_width/height            */

    /* transform */
    ml_rga_rotate_t rotate;
    ml_rga_flip_t   flip;

    /* misc */
    int             dither;       /* enable dither for bit-depth reduction */
} ml_rga_op_t;

#define ML_RGA_OP_DEFAULT {  \
    -1, NULL, 0, 0, 0, 0, 0,   \
    0, 0, 0, 0,                \
    -1, NULL, 0, 0, 0, 0, 0,   \
    0, 0, 0, 0,                \
    ML_RGA_ROTATE_0,            \
    ML_RGA_FLIP_NONE,           \
    0                           \
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

/* Create RGA context.  pool is used for temporary allocations.
 * Returns NULL if RGA hardware is not available. */
ml_rga_t *ml_rga_create(ml_pool_t *pool);

/* Destroy RGA context. Safe on NULL. */
void      ml_rga_destroy(ml_rga_t *rga);

/* ── core operation ───────────────────────────────────────────────────── */

/* Execute one RGA operation (blit + scale + CSC + rotate + flip).
 * Synchronous: blocks until the operation completes.
 * DMA-BUF path: if both src_fd and dst_fd are valid, uses zero-copy.
 * Set src_size/dst_size >0 to enable buffer-capacity validation.
 * Returns 0 on success, -1 on error. */
int  ml_rga_execute(ml_rga_t *rga, const ml_rga_op_t *op);

/* Frame-aware safe wrapper: derives fd/data/size/format/stride from
 * ml_frame_t, validates buffer capacity, and executes the RGA operation.
 * dst_w/dst_h: desired output size (0 = same as src).
 * The caller must ensure dst buffer is pre-allocated with sufficient space.
 * Returns 0 on success, -1 on error. */
int  ml_rga_execute_frame(ml_rga_t *rga,
                           const ml_frame_t *src, ml_frame_t *dst,
                           int dst_w, int dst_h);

/* ── convenience wrappers ─────────────────────────────────────────────── */

/* Resize image (no format change).
 *   rga:     RGA context
 *   src:     source data pointer
 *   src_w/h: source dimensions
 *   dst:     destination buffer (pre-allocated)
 *   dst_w/h: destination dimensions
 *   fmt:     pixel format (must match src & dst)
 * Returns 0 on success, -1 on error. */
int  ml_rga_resize(ml_rga_t *rga,
                   const void *src, int src_w, int src_h,
                   void *dst, int dst_w, int dst_h,
                   ml_rga_format_t fmt);

/* Color-space conversion (no resize).
 *   rga:       RGA context
 *   src:       source data
 *   w, h:      dimensions (same for src & dst)
 *   src_fmt:   input format
 *   dst:       destination buffer (pre-allocated)
 *   dst_fmt:   output format
 * Returns 0 on success, -1 on error. */
int  ml_rga_csc(ml_rga_t *rga,
                const void *src, int w, int h, ml_rga_format_t src_fmt,
                void *dst, ml_rga_format_t dst_fmt);

/* ── CPU fallback for CSC (workaround for RGA kernel driver bug) ──────── */

/* Convert RGB888 or BGR888 to NV12 using CPU (BT.601 coefficients).
 * No RGA hardware involved — safe on all platforms.
 *   src:       input RGB/BGR pixel data
 *   w, h:      width and height (both must be even)
 *   src_fmt:   ML_RGA_FMT_RGB888 or ML_RGA_FMT_BGR888
 *   y_plane:   output Y plane buffer (w*h bytes)
 *   uv_plane:  output UV plane buffer (w*h/2 bytes)
 *   y_stride:  Y plane stride in bytes (>= w, 4-byte aligned)
 * Returns 0 on success, -1 if parameters are invalid. */
int  ml_rga_rgb_to_nv12_cpu(const void *src, int w, int h,
                             ml_rga_format_t src_fmt,
                             uint8_t *y_plane, uint8_t *uv_plane,
                             int y_stride);

/* CPU-only NV12 -> BGR888 bilinear resize + CSC (BT.601 full-range).
 * Safe when RGA virtual-address path is unavailable (src/dst heap
 * buffers above 4G boundary that RK3588 RGA MMU cannot translate).
 *   y_plane:   NV12 Y plane (src_w x src_h)
 *   uv_plane:  NV12 UV plane (interleaved, half-resolution)
 *   src_w, src_h: source dimensions (both must be >= 2)
 *   y_stride:  row stride in bytes for Y plane
 *   uv_stride: row stride in bytes for UV plane (may differ from y_stride)
 *   dst:       output BGR888 buffer (pre-allocated, dst_w * dst_h * 3)
 *   dst_w, dst_h: output dimensions
 * Returns 0 on success, -1 on invalid parameters. */
int  ml_rga_nv12_to_bgr_cpu(const uint8_t *y_plane, const uint8_t *uv_plane,
                             int src_w, int src_h,
                             int y_stride, int uv_stride,
                             uint8_t *dst, int dst_w, int dst_h);

/* NV12 → BGR888 CSC+resize using RGA im2d imcvtcolor with full-range BT.601.
 *
 * Bypasses the legacy c_RkRgaBlit path because its color_space_mode field
 * is silently ignored on RK3588.  The im2d API properly supports
 * IM_YUV_TO_RGB_BT601_FULL mode.
 *
 *   rga:            RGA context
 *   src_fd:         DMA-BUF fd for NV12 buffer, or -1 for virtual memory
 *   src_data:       CPU pointer (used only when src_fd < 0)
 *   src_w, src_h:   image dimensions
 *   src_hor_stride: horizontal stride in pixels (alignment padding per row)
 *   src_ver_stride: vertical stride (MPP_ALIGN(h,16) — used to locate UV plane)
 *   dst:            output BGR888 buffer (pre-allocated, dst_w * dst_h * 3 bytes)
 *   dst_w, dst_h:   output dimensions (may differ from src — includes resize)
 * Returns 0 on success, -1 on error. */
int  ml_rga_nv12_to_bgr(ml_rga_t *rga,
                         int src_fd, const void *src_data,
                         int src_w, int src_h,
                         int src_hor_stride, int src_ver_stride,
                         void *dst, int dst_w, int dst_h);

/* NV16 → BGR888 using im2d (full-range BT.601).  Same as nv12_to_bgr but
 * for 4:2:2 semi-planar YUV.  src_ver_stride is typically = src_h (aligned). */
int  ml_rga_nv16_to_bgr(ml_rga_t *rga,
                         int src_fd, const void *src_data,
                         int src_w, int src_h,
                         int src_hor_stride, int src_ver_stride,
                         void *dst, int dst_w, int dst_h);

/* ── query ────────────────────────────────────────────────────────────── */

/* Last error message. Returns NULL if no error. */
const char *ml_rga_error(ml_rga_t *rga);

/* Return 1 if RGA hardware is available on this system. */
int  ml_rga_available(void);

#ifdef __cplusplus
}
#endif

#endif /* __ML_RGA_H__ */
