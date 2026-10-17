/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_rga.c — Rockchip RGA 2D accelerator implementation.
 *
 * Uses librga.so to offload resize, CSC, rotation, and flip to the
 * RGA hardware engine.  Prefers DMA-BUF zero-copy when both src and
 * dst are DMA-BUF backed; otherwise falls back to virtual-address mode.
 *
 * Compiles only on Linux with librga installed.
 *
 * Safety markers for check-rga-safety.sh:
 *   RGA_SAFETY_MJPEG_DECODED   — MJPEG references are in rejection/error paths
 *   RGA_SAFETY_EXPECT_STRIDE_REJECT — stride expressions are in validation/init code
 */

#include "platform/ml_rga.h"
#include "memory/ml_frame.h"
#include "memory/ml_pool.h"

#include <rga/RgaApi.h>
#include <rga/RgaUtils.h>
#include <rga/rga.h>
#include <rga/im2d.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── module-level init counter (RGA driver is global) ─────────────────── */

static int            rga_init_count = 0;
static pthread_mutex_t rga_init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── internal struct ──────────────────────────────────────────────────── */

struct ml_rga_s {
    ml_pool_t *pool;
    int        inited;
    int        version;          /* RGA driver version    */
    char       err_msg[256];
};

/* ── helpers ──────────────────────────────────────────────────────────── */

static void set_err(ml_rga_t *rga, const char *msg)
{
    if (rga && msg) {
        strncpy(rga->err_msg, msg, sizeof(rga->err_msg) - 1);
        rga->err_msg[sizeof(rga->err_msg) - 1] = '\0';
    }
}

/* convert ml_rga_format_t to RGA format constant */
static int to_rga_format(ml_rga_format_t fmt)
{
    switch (fmt) {
    case ML_RGA_FMT_BGR888:   return RK_FORMAT_BGR_888;
    case ML_RGA_FMT_RGB888:   return RK_FORMAT_RGB_888;
    case ML_RGA_FMT_NV12:     return RK_FORMAT_YCbCr_420_SP;
    case ML_RGA_FMT_NV16:     return RK_FORMAT_YCbCr_422_SP;
    case ML_RGA_FMT_NV21:     return RK_FORMAT_YCrCb_420_SP;
    case ML_RGA_FMT_RGBA8888: return RK_FORMAT_RGBA_8888;
    case ML_RGA_FMT_BGRA8888: return RK_FORMAT_BGRA_8888;
    default:                  return -1;
    }
}

static int to_rga_rotate(ml_rga_rotate_t r)
{
    switch (r) {
    case ML_RGA_ROTATE_90:  return HAL_TRANSFORM_ROT_90;
    case ML_RGA_ROTATE_180: return HAL_TRANSFORM_ROT_180;
    case ML_RGA_ROTATE_270: return HAL_TRANSFORM_ROT_270;
    default:                return 0;
    }
}

static int to_rga_flip(ml_rga_flip_t f)
{
    switch (f) {
    case ML_RGA_FLIP_HORIZONTAL: return HAL_TRANSFORM_FLIP_H;
    case ML_RGA_FLIP_VERTICAL:   return HAL_TRANSFORM_FLIP_V;
    case ML_RGA_FLIP_BOTH:       return HAL_TRANSFORM_FLIP_H_V;
    default:                     return 0;
    }
}

/* convert ml_pixfmt_t (frame-level) to ml_rga_format_t (RGA-level).
 * Rejects compressed formats (MJPEG) and unsupported pixel layouts. */
static int pixfmt_to_rga_fmt(ml_pixfmt_t fmt)
{
    switch (fmt) {
    case ML_PIXFMT_BGR888: return ML_RGA_FMT_BGR888;
    case ML_PIXFMT_RGB888: return ML_RGA_FMT_RGB888;
    case ML_PIXFMT_NV12:   return ML_RGA_FMT_NV12;
    case ML_PIXFMT_NV16:   return ML_RGA_FMT_NV16;
    case ML_PIXFMT_NV21:   return ML_RGA_FMT_NV21;
    default:               return -1;
    }
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

ml_rga_t *
ml_rga_create(ml_pool_t *pool)
{
    ml_rga_t *rga;

    if (!ml_rga_available())
        return NULL;
    if (!pool)
        return NULL;

    rga = pool->f_alloc(pool, sizeof(*rga));
    if (!rga)
        return NULL;
    memset(rga, 0, sizeof(*rga));

    rga->pool = pool;

    pthread_mutex_lock(&rga_init_mutex);
    if (rga_init_count == 0) {
        if (c_RkRgaInit() < 0) {
            pthread_mutex_unlock(&rga_init_mutex);
            set_err(rga, "RGA init failed");
            pool->f_free(pool, rga);
            return NULL;
        }
    }
    rga_init_count++;
    rga->inited = 1;
    pthread_mutex_unlock(&rga_init_mutex);

    rga->version = RGA_API_MAJOR_VERSION * 10000
                 + RGA_API_MINOR_VERSION * 100
                 + RGA_API_REVISION_VERSION;

    return rga;
}

void
ml_rga_destroy(ml_rga_t *rga)
{
    if (!rga)
        return;
    if (rga->inited) {
        pthread_mutex_lock(&rga_init_mutex);
        if (rga_init_count == 1)
            c_RkRgaDeInit();
        if (rga_init_count > 0)
            rga_init_count--;
        pthread_mutex_unlock(&rga_init_mutex);
    }
    if (rga->pool) rga->pool->f_free(rga->pool, rga);
}

/* ── validation ────────────────────────────────────────────────────────── */

/* Compute minimum buffer size in bytes for given RGA format and dimensions.
 * For NV12/NV21: returns Y+UV total (stride * height * 3/2).
 * For packed RGB: returns stride * height * bpp.
 * Returns 0 for unknown formats. */
static size_t rga_min_size(int width, int height, int stride, ml_rga_format_t fmt)
{
    switch (fmt) {
    case ML_RGA_FMT_BGR888:
    case ML_RGA_FMT_RGB888:
        return (size_t)stride * height * 3;
    case ML_RGA_FMT_RGBA8888:
    case ML_RGA_FMT_BGRA8888:
        return (size_t)stride * height * 4;
    case ML_RGA_FMT_NV12:
    case ML_RGA_FMT_NV21:
        return (size_t)stride * height * 3 / 2;
    case ML_RGA_FMT_NV16:
        return (size_t)stride * height * 2;  /* Y + UV, each stride*height */
    default:
        return 0;
    }
}

/* Validate operation parameters before touching hardware.
 * Catches common mistakes that cause IOMMU faults / kernel panics:
 *   - DRM dumb-buffer byte-stride passed as pixel-stride
 *   - buffer too small for claimed format/dimensions (when size > 0)
 *   - negative dimensions
 *   - stale/invalid DMA-BUF fds
 *   - missing data pointer in virtual-address mode
 *
 * Called by ml_rga_execute — public API validates at entry, so
 * internal convenience wrappers are also protected. */
static int ml_rga_op_validate(ml_rga_t *rga, const ml_rga_op_t *op)
{
    int src_stride, dst_stride;
    int src_w, src_h, dst_w, dst_h;
    size_t min_src, min_dst;

    src_w = op->src_w ? op->src_w : op->src_width;
    src_h = op->src_h ? op->src_h : op->src_height;
    dst_w = op->dst_w ? op->dst_w : op->dst_width;
    dst_h = op->dst_h ? op->dst_h : op->dst_height;

    src_stride = op->src_stride ? op->src_stride : src_w;
    dst_stride = op->dst_stride ? op->dst_stride : dst_w;

    /* ── dimensions ──────────────────────────────────────────────────── */

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        set_err(rga, "invalid dimensions (width/height must be > 0)");
        return -1;
    }

    /* ── stride sanity ─────────────────────────────────────────────────
     * The RGA driver (and our public API) expects stride in *pixels*.
     * DRM dumb buffers use byte-pitch which can be 4-8× larger.
     * A pixel-stride > 8× width almost certainly means byte-stride was
     * passed by mistake — reject it before the hardware page-faults. */

    if (src_stride < src_w) {
        set_err(rga, "src_stride < src_width");
        return -1;
    }
    if (dst_stride < dst_w) {
        set_err(rga, "dst_stride < dst_width");
        return -1;
    }
    if (src_stride > src_w * 8) {
        set_err(rga, "src_stride suspiciously large — byte-stride passed as pixel-stride?");
        return -1;
    }
    if (dst_stride > dst_w * 8) {
        set_err(rga, "dst_stride suspiciously large — byte-stride passed as pixel-stride?");
        return -1;
    }

    /* ── format ──────────────────────────────────────────────────────── */

    if (to_rga_format(op->src_format) < 0) {
        set_err(rga, "unsupported src format");
        return -1;
    }
    if (to_rga_format(op->dst_format) < 0) {
        set_err(rga, "unsupported dst format");
        return -1;
    }

    /* ── buffer capacity (when caller provides size) ───────────────────
     * Optional: if src_size/dst_size is 0 the check is skipped.
     * When set, the declared buffer must be large enough for the claimed
     * format × dimensions × stride.  This catches the "MJPEG compressed
     * data passed as BGR888" class of bugs at the API boundary. */

    if (op->src_size > 0) {
        min_src = rga_min_size(src_w, src_h, src_stride, op->src_format);
        if (min_src > 0 && op->src_size < min_src) {
            set_err(rga, "src_size too small for claimed format/dimensions");
            return -1;
        }
    }
    if (op->dst_size > 0) {
        min_dst = rga_min_size(dst_w, dst_h, dst_stride, op->dst_format);
        if (min_dst > 0 && op->dst_size < min_dst) {
            set_err(rga, "dst_size too small for claimed format/dimensions");
            return -1;
        }
    }

    /* ── DMA-BUF fd liveness ─────────────────────────────────────────── */

    if (op->src_fd >= 0) {
        if (fcntl(op->src_fd, F_GETFD) == -1) {
            set_err(rga, "src_fd is not a valid file descriptor");
            return -1;
        }
    }
    if (op->dst_fd >= 0) {
        if (fcntl(op->dst_fd, F_GETFD) == -1) {
            set_err(rga, "dst_fd is not a valid file descriptor");
            return -1;
        }
    }

    /* ── virtual-address mode: data pointer required ─────────────────── */

    if (op->src_fd < 0 && !op->src_data) {
        set_err(rga, "src: must provide data or fd");
        return -1;
    }
    if (op->dst_fd < 0 && !op->dst_data) {
        set_err(rga, "dst: must provide data or fd");
        return -1;
    }

    /* RK3588 RGA MMU cannot translate virtual addresses >= 4 GiB.
     * Passing such addresses triggers "unsupported memory larger than 4G"
     * in the kernel driver -> failed job commit -> leaked MM reference ->
     * corrupted rss-counter / pgtables -> kernel panic.
     * Callers must use DMA-BUF (fd >= 0) or a CPU fallback. */
    if (op->src_fd < 0 && op->src_data
        && ((uintptr_t)op->src_data) >= 0x100000000ULL) {
        set_err(rga, "src_data addr >= 4G, unsafe for RGA MMU; use DMA-BUF or CPU fallback");
        return -1;
    }
    if (op->dst_fd < 0 && op->dst_data
        && ((uintptr_t)op->dst_data) >= 0x100000000ULL) {
        set_err(rga, "dst_data addr >= 4G, unsafe for RGA MMU; use DMA-BUF or CPU fallback");
        return -1;
    }

    return 0;
}

/* ── execute ──────────────────────────────────────────────────────────── */

int
ml_rga_execute(ml_rga_t *rga, const ml_rga_op_t *op)
{
    rga_info_t src, dst;
    int src_fmt, dst_fmt;
    int src_w, src_h, dst_w, dst_h;
    int ret;

    if (!rga || !rga->inited || !op)
        return -1;

    if (ml_rga_op_validate(rga, op) != 0)
        return -1;

    src_fmt = to_rga_format(op->src_format);
    dst_fmt = to_rga_format(op->dst_format);

    src_w = op->src_w ? op->src_w : op->src_width;
    src_h = op->src_h ? op->src_h : op->src_height;
    dst_w = op->dst_w ? op->dst_w : op->dst_width;
    dst_h = op->dst_h ? op->dst_h : op->dst_height;

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    /* source: virtual-address or DMA-BUF, reject invalid combo */
    src.fd = op->src_fd;
    if (op->src_fd < 0) {
        if (!op->src_data) {
            set_err(rga, "src: must provide data or fd");
            return -1;
        }
        src.virAddr = (void *)op->src_data;
        src.mmuFlag = 0; /* virtual address mode */
    } else {
        src.mmuFlag = 1; /* DMA-BUF mode */
    }
    rga_set_rect(&src.rect, op->src_x, op->src_y,
                 src_w, src_h,
                 op->src_stride ? op->src_stride : src_w,
                 src_h, src_fmt);
    src.format = src_fmt;  /* librga2 c_RkRgaBlit reads top-level format,
                             * not rect.format. Without this, driver sees
                             * format=0 (RGBA_8888) → "Invalid argument". */

    /* destination */
    dst.fd = op->dst_fd;
    if (op->dst_fd < 0) {
        if (!op->dst_data) {
            set_err(rga, "dst: must provide data or fd");
            return -1;
        }
        dst.virAddr = op->dst_data;
        dst.mmuFlag = 0;
    } else {
        dst.mmuFlag = 1;
    }
    rga_set_rect(&dst.rect, op->dst_x, op->dst_y,
                 dst_w, dst_h,
                 op->dst_stride ? op->dst_stride : dst_w,
                 dst_h, dst_fmt);
    dst.format = dst_fmt;

    /* transform */
    src.rotation = to_rga_rotate(op->rotate) | to_rga_flip(op->flip);

    /* dither */
    if (op->dither)
        src.dither.enable = 1;

    /* ── Color space: YUV full-range (JPEG) for camera sources ──────
     *
     * RGA old-API color_space_mode (rga_info_t):
     *   0 = yuv2rgb_mode0 — BT.601 MPEG limited-range
     *       Y [16,235] → RGB [0,255] → washed-out for JPEG sources
     *   1 = yuv2rgb_mode1 — BT.601 JPEG full-range
     *       Y [0,255] → RGB [0,255] → correct for MJPEG camera frames
     *
     * This matches ml_rga_nv12_to_bgr() which uses the im2d API with
     * IM_YUV_TO_RGB_BT601_FULL. */
    if (src_fmt != dst_fmt) {
        int yuv_fmts = (src_fmt == RK_FORMAT_YCbCr_420_SP ||
                        src_fmt == RK_FORMAT_YCbCr_422_SP ||
                        src_fmt == RK_FORMAT_YCrCb_420_SP);
        int rgb_fmts = (dst_fmt == RK_FORMAT_RGB_888  ||
                        dst_fmt == RK_FORMAT_BGR_888  ||
                        dst_fmt == RK_FORMAT_RGBA_8888 ||
                        dst_fmt == RK_FORMAT_BGRA_8888);
        if (yuv_fmts && rgb_fmts)
            src.color_space_mode = 1; /* yuv2rgb_mode1: BT.601 full-range */
    }

    ret = c_RkRgaBlit(&src, &dst, NULL);
    if (ret < 0) {
        set_err(rga, "RGA blit failed");
        return -1;
    }

    return 0;
}

/* ── convenience wrappers ─────────────────────────────────────────────── */

int
ml_rga_resize(ml_rga_t *rga,
              const void *src, int src_w, int src_h,
              void *dst, int dst_w, int dst_h,
              ml_rga_format_t fmt)
{
    ml_rga_op_t op = ML_RGA_OP_DEFAULT;

    op.src_data   = src;
    op.src_width  = src_w;
    op.src_height = src_h;
    op.src_format = fmt;
    op.src_w      = src_w;
    op.src_h      = src_h;

    op.dst_data   = dst;
    op.dst_width  = dst_w;
    op.dst_height = dst_h;
    op.dst_format = fmt;
    op.dst_w      = dst_w;
    op.dst_h      = dst_h;

    op.src_size   = rga_min_size(src_w, src_h, src_w, fmt);
    op.dst_size   = rga_min_size(dst_w, dst_h, dst_w, fmt);

    return ml_rga_execute(rga, &op);
}

int
ml_rga_csc(ml_rga_t *rga,
           const void *src, int w, int h, ml_rga_format_t src_fmt,
           void *dst, ml_rga_format_t dst_fmt)
{
    ml_rga_op_t op = ML_RGA_OP_DEFAULT;

    op.src_data   = src;
    op.src_width  = w;
    op.src_height = h;
    op.src_format = src_fmt;
    op.src_w      = w;
    op.src_h      = h;

    op.dst_data   = dst;
    op.dst_width  = w;
    op.dst_height = h;
    op.dst_format = dst_fmt;
    op.dst_w      = w;
    op.dst_h      = h;

    op.src_size   = rga_min_size(w, h, w, src_fmt);
    op.dst_size   = rga_min_size(w, h, w, dst_fmt);

    return ml_rga_execute(rga, &op);
}

/* ── frame-aware execution ─────────────────────────────────────────────── */

int
ml_rga_execute_frame(ml_rga_t *rga,
                     const ml_frame_t *src, ml_frame_t *dst,
                     int dst_w, int dst_h)
{
    ml_rga_op_t op = ML_RGA_OP_DEFAULT;
    int rga_fmt;
    int width, height;

    if (!rga || !rga->inited || !src || !dst)
        return -1;

    width  = (int)src->width;
    height = (int)src->height;
    if (dst_w <= 0) dst_w = width;
    if (dst_h <= 0) dst_h = height;

    /* reject compressed formats — RGA operates on raw pixels only */
    rga_fmt = pixfmt_to_rga_fmt((ml_pixfmt_t)src->format);
    if (rga_fmt < 0) {
        set_err(rga, "frame src format unsupported by RGA (must be raw pixels, not MJPEG/H264)");
        return -1;
    }
    op.src_format = (ml_rga_format_t)rga_fmt;
    op.dst_format = (ml_rga_format_t)rga_fmt;

    /* source: prefer DMA-BUF, fall back to virtual-address */
    if ((src->flags & ML_FRAME_FLAG_DMABUF) && src->fd[0] >= 0) {
        op.src_fd   = src->fd[0];
        op.src_size = src->size[0];
    } else if (src->data[0]) {
        op.src_data = src->data[0];
        op.src_size = src->size[0];
    } else {
        set_err(rga, "frame src has no data or DMA-BUF fd");
        return -1;
    }
    op.src_width  = width;
    op.src_height = height;

    /* Frame stride is in bytes (ml_frame convention); convert to RGA pixel stride.
     * For NV12: bpp=8 → pixel_stride = byte_stride.
     * For BGR888: bpp=24 → pixel_stride = byte_stride / 3. */
    {
        int bpp = ml_pixfmt_bpp_total((ml_pixfmt_t)src->format);
        if (bpp > 0 && src->stride[0] > 0)
            op.src_stride = (int)(src->stride[0] / (uint32_t)(bpp / 8));
        else
            op.src_stride = width;
    }

    /* destination */
    if ((dst->flags & ML_FRAME_FLAG_DMABUF) && dst->fd[0] >= 0) {
        op.dst_fd   = dst->fd[0];
        op.dst_size = dst->size[0];
    } else if (dst->data[0]) {
        op.dst_data = dst->data[0];
        op.dst_size = dst->size[0];
    } else {
        set_err(rga, "frame dst has no data or DMA-BUF fd");
        return -1;
    }
    op.dst_width  = dst_w;
    op.dst_height = dst_h;
    op.dst_stride = dst_w; /* output stride = pixel width for packed formats */

    return ml_rga_execute(rga, &op);
}

/* ── CPU fallback for CSC (workaround for RGA kernel driver bug) ──────── */

int
ml_rga_rgb_to_nv12_cpu(const void *src, int w, int h,
                        ml_rga_format_t src_fmt,
                        uint8_t *y_plane, uint8_t *uv_plane,
                        int y_stride)
{
    const uint8_t *rgb = (const uint8_t *)src;
    int r_off, g_off, b_off;
    int x, y;

    if (!src || !y_plane || !uv_plane)
        return -1;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return -1;  /* must be even */
    if (y_stride < w)
        return -1;

    /* determine byte offsets for R, G, B within each pixel triplet */
    switch (src_fmt) {
    case ML_RGA_FMT_BGR888:
        b_off = 0; g_off = 1; r_off = 2; break;
    case ML_RGA_FMT_RGB888:
    default:
        r_off = 0; g_off = 1; b_off = 2; break;
    }

    /* Y plane — full resolution, BT.601 coefficients */
    for (y = 0; y < h; y++) {
        const uint8_t *row = rgb + y * w * 3;
        uint8_t       *ydst = y_plane + y * y_stride;

        for (x = 0; x < w; x++) {
            int r = row[x * 3 + r_off];
            int g = row[x * 3 + g_off];
            int b = row[x * 3 + b_off];
            int yy = (77 * r + 150 * g + 29 * b) >> 8;
            ydst[x] = (uint8_t)(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
        }
    }

    /* UV plane — 2x2 downsampled, BT.601 coefficients */
    for (y = 0; y < h / 2; y++) {
        const uint8_t *row0 = rgb + y * 2 * w * 3;
        const uint8_t *row1 = row0 + w * 3;
        uint8_t       *uv   = uv_plane + y * y_stride;

        for (x = 0; x < w / 2; x++) {
            int x2 = x * 2;
            int r = (row0[(x2)   * 3 + r_off] + row0[(x2+1) * 3 + r_off] +
                     row1[(x2)   * 3 + r_off] + row1[(x2+1) * 3 + r_off]) / 4;
            int g = (row0[(x2)   * 3 + g_off] + row0[(x2+1) * 3 + g_off] +
                     row1[(x2)   * 3 + g_off] + row1[(x2+1) * 3 + g_off]) / 4;
            int b = (row0[(x2)   * 3 + b_off] + row0[(x2+1) * 3 + b_off] +
                     row1[(x2)   * 3 + b_off] + row1[(x2+1) * 3 + b_off]) / 4;

            int u = (-43 * r - 85 * g + 128 * b + 128) >> 8;
            int v = (128 * r - 107 * g - 21 * b + 128) >> 8;

            uv[x * 2]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            uv[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }

    return 0;
}

/* ── CPU fallback: NV12 -> BGR888 bilinear resize (BT.601 full) ---------
	 *
	 * Safe alternative when RGA virtual-address path is unavailable
	 * (e.g. src/dst heap buffers above 4G boundary that RK3588 RGA
	 * MMU cannot translate).  Uses BT.601 full-range (JPEG 0-255 Y)
	 * coefficients.  Caller must pre-allocate dst (dst_w * dst_h * 3).
	 *
	 * y_stride:  row stride in bytes for Y plane
	 * uv_stride: row stride in bytes for UV plane (may differ from Y stride
	 *            in real NV12 frames; equals roiW for pose packed-crop path) */
	int
	ml_rga_nv12_to_bgr_cpu(const uint8_t *y_plane, const uint8_t *uv_plane,
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

				/* bilinear interpolation on Y plane */
				float y00 = (float)y_plane[sy0 * y_stride + sx0];
				float y10 = (float)y_plane[sy0 * y_stride + sx0 + 1];
				float y01 = (float)y_plane[(sy0 + 1) * y_stride + sx0];
				float y11 = (float)y_plane[(sy0 + 1) * y_stride + sx0 + 1];
				float Y = y00 * (1.0f - sx_frac) * (1.0f - sy_frac)
				        + y10 * sx_frac * (1.0f - sy_frac)
				        + y01 * (1.0f - sx_frac) * sy_frac
				        + y11 * sx_frac * sy_frac;

				/* nearest-neighbour on UV plane (half-resolution) */
				int uv_x = sx0 / 2;
				int uv_y = sy0 / 2;
				float U = (float)uv_plane[uv_y * uv_stride + uv_x * 2];
				float V = (float)uv_plane[uv_y * uv_stride + uv_x * 2 + 1];

				/* BT.601 full-range (JPEG) YUV -> BGR */
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


/* ── NV12 → BGR888 via im2d imcvtcolor (full-range BT.601) ─────────── */

int
ml_rga_nv12_to_bgr(ml_rga_t *rga,
                    int src_fd, const void *src_data,
                    int src_w, int src_h,
                    int src_hor_stride, int src_ver_stride,
                    void *dst, int dst_w, int dst_h)
{
    rga_buffer_t src_buf, dst_buf;
    IM_STATUS ret;

    if (!rga || !rga->inited || !dst)
        return -1;

    /* Source: NV12 semi-planar in DMA-BUF or virtual memory */
    if (src_fd >= 0) {
        src_buf = wrapbuffer_fd_t(src_fd, src_w, src_h,
                                  src_hor_stride, src_ver_stride,
                                  RK_FORMAT_YCbCr_420_SP);
    } else {
        if (!src_data)
            return -1;
        src_buf = wrapbuffer_virtualaddr_t((void *)src_data, src_w, src_h,
                                            src_hor_stride, src_ver_stride,
                                            RK_FORMAT_YCbCr_420_SP);
    }

    /* Destination: BGR888 packed, virtual memory */
    dst_buf = wrapbuffer_virtualaddr_t(dst, dst_w, dst_h,
                                        dst_w, dst_h,
                                        RK_FORMAT_BGR_888);

    ret = imcvtcolor_t(src_buf, dst_buf,
                       RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888,
                       IM_YUV_TO_RGB_BT601_FULL, 1);
    if (ret != IM_STATUS_SUCCESS) {
        set_err(rga, "imcvtcolor NV12→BGR failed");
        return -1;
    }
    return 0;
}

int
ml_rga_nv12_to_bgr_dmabuf(ml_rga_t *rga,
                           int src_fd, int src_w, int src_h,
                           int src_hor_stride, int src_ver_stride,
                           int dst_fd, int dst_w, int dst_h)
{
    rga_buffer_t src_buf, dst_buf;
    IM_STATUS ret;

    if (!rga || !rga->inited || src_fd < 0 || dst_fd < 0 ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        src_hor_stride < src_w || src_ver_stride < src_h)
        return -1;

    src_buf = wrapbuffer_fd_t(src_fd, src_w, src_h,
                              src_hor_stride, src_ver_stride,
                              RK_FORMAT_YCbCr_420_SP);
    dst_buf = wrapbuffer_fd_t(dst_fd, dst_w, dst_h,
                              dst_w, dst_h, RK_FORMAT_BGR_888);
    ret = imcvtcolor_t(src_buf, dst_buf,
                       RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888,
                       IM_YUV_TO_RGB_BT601_FULL, 1);
    if (ret != IM_STATUS_SUCCESS) {
        set_err(rga, "imcvtcolor DMA-BUF NV12->BGR failed");
        return -1;
    }
    return 0;
}

/* ── NV16 → BGR888 via im2d imcvtcolor (full-range BT.601) ──────────── */

int
ml_rga_nv16_to_bgr(ml_rga_t *rga,
                    int src_fd, const void *src_data,
                    int src_w, int src_h,
                    int src_hor_stride, int src_ver_stride,
                    void *dst, int dst_w, int dst_h)
{
    rga_buffer_t src_buf, dst_buf;
    IM_STATUS ret;

    if (!rga || !rga->inited || !dst)
        return -1;

    if (src_fd >= 0) {
        src_buf = wrapbuffer_fd_t(src_fd, src_w, src_h,
                                  src_hor_stride, src_ver_stride,
                                  RK_FORMAT_YCbCr_422_SP);
    } else {
        if (!src_data)
            return -1;
        src_buf = wrapbuffer_virtualaddr_t((void *)src_data, src_w, src_h,
                                            src_hor_stride, src_ver_stride,
                                            RK_FORMAT_YCbCr_422_SP);
    }

    dst_buf = wrapbuffer_virtualaddr_t(dst, dst_w, dst_h,
                                        dst_w, dst_h,
                                        RK_FORMAT_BGR_888);

    ret = imcvtcolor_t(src_buf, dst_buf,
                       RK_FORMAT_YCbCr_422_SP, RK_FORMAT_BGR_888,
                       IM_YUV_TO_RGB_BT601_FULL, 1);
    if (ret != IM_STATUS_SUCCESS) {
        set_err(rga, "imcvtcolor NV16→BGR failed");
        return -1;
    }
    return 0;
}

/* ── query ────────────────────────────────────────────────────────────── */

const char *
ml_rga_error(ml_rga_t *rga)
{
    if (!rga || !rga->err_msg[0])
        return NULL;
    return rga->err_msg;
}

int
ml_rga_available(void)
{
    /* check if RGA device node exists */
    return (access("/dev/rga", F_OK) == 0) ? 1 : 0;
}
