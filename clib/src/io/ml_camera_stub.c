/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_camera_stub.c — Camera stub backend (fallback for unsupported platforms).
 *
 * All operations return error codes.  Used when V4L2 or RTSP backends are
 * unavailable.  The dispatcher (ml_camera.c) delegates BACKEND_FILE and
 * BACKEND_IMAGE here, plus any backend not compiled on the current platform.
 */

#include "io/ml_camera_private.h"
#include "memory/ml_pool.h"

#include <stdlib.h>
#include <stdio.h>

static void stub_destroy(ml_camera_t *cam) {
    if (cam && cam->pool) cam->pool->f_free(cam->pool, cam);
}
static int  stub_start(ml_camera_t *cam) { (void)cam; return -1; }
static int  stub_stop(ml_camera_t *cam) { (void)cam; return -1; }
static int  stub_read(ml_camera_t *cam, ml_frame_t *f, int to)
{ (void)cam; (void)f; (void)to; return -1; }
static void stub_frame_release(ml_camera_t *cam, ml_frame_t *f)
{ (void)cam; (void)f; }
static int  stub_set_callback(ml_camera_t *cam, ml_camera_frame_cb cb, void *d)
{ (void)cam; (void)cb; (void)d; return -1; }

static int  stub_get_width(ml_camera_t *cam)  { (void)cam; return -1; }
static int  stub_get_height(ml_camera_t *cam) { (void)cam; return -1; }
static int  stub_get_fps(ml_camera_t *cam)    { (void)cam; return -1; }
static int  stub_get_state(ml_camera_t *cam)  { return cam ? cam->state : -1; }

static const char *stub_error(ml_camera_t *cam)
{
    if (!cam || !cam->err_msg[0])
        return NULL;
    return cam->err_msg;
}

const ml_camera_ops_t ml_camera_stub_ops = {
    .destroy       = stub_destroy,
    .start         = stub_start,
    .stop          = stub_stop,
    .read          = stub_read,
    .frame_release = stub_frame_release,
    .set_callback  = stub_set_callback,
    .get_width     = stub_get_width,
    .get_height    = stub_get_height,
    .get_fps       = stub_get_fps,
    .get_state     = stub_get_state,
    .error         = stub_error,
};

ml_camera_t *
ml_camera_stub_create(const ml_camera_cfg_t *cfg, ml_pool_t *pool)
{
    ml_camera_t *cam;

    if (!cfg || !pool)
        return NULL;

    cam = pool->f_alloc(pool, sizeof(*cam));
    if (!cam)
        return NULL;
    memset(cam, 0, sizeof(*cam));

    cam->backend = cfg->backend;
    cam->ops     = &ml_camera_stub_ops;
    cam->cfg     = *cfg;
    cam->pool    = pool;
    cam->state   = ML_CAMERA_STATE_STOPPED;
    snprintf(cam->err_msg, sizeof(cam->err_msg),
             "camera backend %d not available on this platform", cfg->backend);
    return cam;
}
