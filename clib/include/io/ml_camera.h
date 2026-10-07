/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_camera — Unified camera abstraction (V4L2 / RTSP / file / image).
 *
 * Provides a common API for reading frames from USB/MIPI cameras (V4L2),
 * RTSP network cameras, local video files, or still images.
 *
 * Lifecycle:
 *   create → start → read* (or set_callback) → stop → destroy
 *
 * DMA-BUF zero-copy is preferred on V4L2 if the hardware supports it;
 * otherwise falls back to MMAP buffer sharing.
 */

#ifndef __ML_CAMERA_H__
#define __ML_CAMERA_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── forward declarations ─────────────────────────────────────────────── */

typedef struct ml_frame_s   ml_frame_t;
typedef struct ml_pool_s    ml_pool_t;
typedef struct ml_camera_s  ml_camera_t;

/* ── enums ─────────────────────────────────────────────────────────────── */

typedef enum {
    ML_CAMERA_BACKEND_V4L2  = 0,   /* /dev/videoN USB/MIPI camera      */
    ML_CAMERA_BACKEND_RTSP  = 1,   /* rtsp://... network camera        */
    ML_CAMERA_BACKEND_FILE  = 2,   /* raw frame file (NV12/YUV, looped) */
    ML_CAMERA_BACKEND_IMAGE = 3,   /* still image (jpg/png, looped)    */
} ml_camera_backend_t;

typedef enum {
    ML_CAMERA_STATE_STOPPED = 0,
    ML_CAMERA_STATE_RUNNING = 1,
    ML_CAMERA_STATE_ERROR   = 2,
} ml_camera_state_t;

/* ── configuration ────────────────────────────────────────────────────── */

typedef struct {
    ml_camera_backend_t  backend;
    const char          *device;       /* /dev/video0 or rtsp://...    */
    int                  width;
    int                  height;
    int                  fps_num;
    int                  fps_den;
    int                  pixfmt;       /* desired ml_pixfmt_t output    */
    int                  buffer_count; /* V4L2 buffer count (4-8)       */
    int                  rotation;     /* 0, 90, 180, 270               */
    int                  timeout_ms;   /* read timeout, 0 = blocking    */
} ml_camera_cfg_t;

#define ML_CAMERA_CFG_DEFAULT {                \
    ML_CAMERA_BACKEND_V4L2,                    \
    "/dev/video0",                             \
    1920, 1080,                                \
    30, 1,                                     \
    ML_PIXFMT_NV12,                             \
    4,                                         \
    0,                                         \
    3000                                       \
}

/* ── async callback ───────────────────────────────────────────────────── */

/* Called from internal capture thread when a new frame is available.
 * The frame is valid only for the duration of the callback.
 * Return 0 to continue, -1 to stop. */
typedef int (*ml_camera_frame_cb)(ml_frame_t *frame, void *user_data);

/* ── lifecycle ────────────────────────────────────────────────────────── */

/* Create camera from configuration. Returns NULL on failure. */
ml_camera_t *ml_camera_create(const ml_camera_cfg_t *cfg, ml_pool_t *pool);

/* Destroy camera. Stops capture if running. Safe on NULL. */
void         ml_camera_destroy(ml_camera_t *cam);

/* ── capture control ──────────────────────────────────────────────────── */

/* Start capture. Returns 0 on success, -1 on error. */
int  ml_camera_start(ml_camera_t *cam);

/* Stop capture. Returns 0 on success, -1 on error. */
int  ml_camera_stop(ml_camera_t *cam);

/* ── synchronous read ─────────────────────────────────────────────────── */

/* Read one frame (blocking or timeout-controlled).
 * Caller must call ml_camera_frame_release when done.
 * Returns 0 on success, -1 on timeout/error. */
int  ml_camera_read(ml_camera_t *cam, ml_frame_t *frame, int timeout_ms);

/* Return a frame's buffer to the driver pool. Required after every
 * ml_camera_read. Safe to call on NULL. */
void ml_camera_frame_release(ml_camera_t *cam, ml_frame_t *frame);

/* ── async callback mode ──────────────────────────────────────────────── */

/* Set async frame callback. Replaces any previous callback.
 * The camera must be stopped when calling this.
 * Use ml_camera_start to begin the internal capture thread. */
int  ml_camera_set_callback(ml_camera_t *cam, ml_camera_frame_cb cb,
                            void *user_data);

/* ── query ────────────────────────────────────────────────────────────── */

int         ml_camera_get_width(ml_camera_t *cam);
int         ml_camera_get_height(ml_camera_t *cam);
int         ml_camera_get_fps(ml_camera_t *cam);
int         ml_camera_get_state(ml_camera_t *cam);
const char *ml_camera_error(ml_camera_t *cam);

#ifdef __cplusplus
}
#endif

#endif /* __ML_CAMERA_H__ */
