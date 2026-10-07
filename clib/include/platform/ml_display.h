/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_display — hardware display abstraction with OSD overlay.
 *
 * Build-time backends:
 *   - DISPLAY_BACKEND=drm: Linux DRM/KMS dumb-buffer output.
 *   - DISPLAY_BACKEND=sdl: SDL2 window/texture output, including KMSDRM HDMI.
 *
 * The caller uses this API only; it does not include SDL headers.
 */

#ifndef __ML_DISPLAY_H__
#define __ML_DISPLAY_H__

#include <stdint.h>

#include "memory/ml_frame.h"
#include "memory/ml_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declaration ──────────────────────────────────────────── */

typedef struct ml_display_s ml_display_t;

/* ── Configuration ─────────────────────────────────────────────────── */

typedef struct {
    int preferred_w;     /* desired width (0 = auto-match preferred mode)  */
    int preferred_h;     /* desired height (0 = auto-match preferred mode) */
    int preferred_fps;   /* desired refresh rate (0 = auto-match)          */
    int max_w;           /* max resolution cap (0 = no limit, default 1920)*/
    int max_h;           /* max resolution cap (0 = no limit, default 1080)*/
    int display_index;   /* SDL display index; ignored by DRM backend         */
    int fullscreen;      /* SDL fullscreen desktop mode; ignored by DRM      */
} ml_display_cfg_t;

#define ML_DISPLAY_CFG_DEFAULT { 0, 0, 0, 1920, 1080, 0, 1 }

/* ── OSD rectangle ─────────────────────────────────────────────────── */

typedef struct {
    int       x, y, w, h;
    uint8_t   r, g, b;       /* outline colour (sRGB, 0-255) */
    int       thickness;     /* line width in pixels, default 2 */
    char      label[64];     /* label text (empty string = no label) */
} ml_osd_rect_t;

/* ── OSD line segment (for skeleton drawing) ──────────────────────── */

typedef struct {
    int       x1, y1, x2, y2;  /* endpoints in frame-pixel coordinates */
    uint8_t   r, g, b;          /* line colour (sRGB, 0-255) */
    int       thickness;        /* line width in pixels, default 2 */
} ml_osd_line_t;

/* ── OSD point (filled circle for keypoints) ──────────────────────── */

typedef struct {
    int       x, y;             /* centre in frame-pixel coordinates */
    int       radius;            /* radius in pixels, default 3 */
    uint8_t   r, g, b;          /* fill colour (sRGB, 0-255) */
} ml_osd_point_t;

/* ── OSD standalone text (HUD overlay: FPS, timestamp, etc.) ─────── */

typedef struct {
    int       x, y;             /* top-left in frame-pixel coordinates */
    uint8_t   r, g, b;          /* text colour (sRGB, 0-255) */
    uint8_t   bg_r, bg_g, bg_b; /* background colour (0=transparent) */
    char      text[128];         /* null-terminated UTF-8 string */
} ml_osd_text_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Create a display context. pool is used for internal allocations; pass
 * NULL to use an internal temporary pool. Returns NULL on allocation
 * failure. */
ml_display_t *ml_display_create(ml_pool_t *pool);

/* Destroy a display context. Safe on NULL. */
void          ml_display_destroy(ml_display_t *d);

/* Initialise — open DRM device, pick display mode, allocate double-buffer
 * framebuffers. Returns 0 on success, -1 on failure (check
 * ml_display_error). */
int  ml_display_init(ml_display_t *d, const ml_display_cfg_t *cfg);

/* ── Frame display ─────────────────────────────────────────────────── */

/* Display a frame with OSD overlay.
 *
 * frame: must carry data[0] (user-space pixels). The format field must be
 *        ML_PIXFMT_RGB888 or ML_PIXFMT_BGR888. stride[0] may differ from
 *        width * bpp/8 (alignment scenarios). The SDL backend renders these
 *        formats directly; it intentionally does not take ownership of frame.
 *
 * rects / n_rects: detection-box overlays (may be NULL / 0).
 *
 * Returns 0 on success, -1 on failure.
 *
 * Contract: this function reads frame data only — it does NOT modify any
 * frame field, does NOT call ml_frame_unref, and does NOT hold a
 * reference. The caller owns the frame exclusively. */
int  ml_display_show_frame(ml_display_t *d, const ml_frame_t *frame,
                           const ml_osd_rect_t *rects, int n_rects,
                           const ml_osd_text_t *texts, int n_texts);

/* Display a frame with full OSD overlay (rects + lines + points + texts).
 *
 * lines / n_lines: skeleton-bone line segments (may be NULL / 0).
 *   Lines are clipped to the display bounds but NOT culled — draw only
 *   bones whose both endpoints have a valid keypoint (score >= threshold).
 *
 * points / n_points: filled-circle keypoint markers (may be NULL / 0).
 *   Points are clipped to the display bounds but NOT culled.
 *
 * texts / n_texts: standalone HUD text (FPS, timestamp, etc.) (may be NULL/0).
 *
 * All coordinates are in frame-pixel space (same as rects) and are
 * transformed to display coordinates using the same scale + offset. */
int  ml_display_show_frame_ex(ml_display_t *d, const ml_frame_t *frame,
                              const ml_osd_rect_t *rects,  int n_rects,
                              const ml_osd_line_t *lines,  int n_lines,
                              const ml_osd_point_t *points, int n_points,
                              const ml_osd_text_t *texts,  int n_texts);

/* ── Queries ───────────────────────────────────────────────────────── */

int         ml_display_get_width(ml_display_t *d);
int         ml_display_get_height(ml_display_t *d);
int         ml_display_is_available(void);
const char *ml_display_error(ml_display_t *d);

#ifdef __cplusplus
}
#endif

#endif /* __ML_DISPLAY_H__ */
