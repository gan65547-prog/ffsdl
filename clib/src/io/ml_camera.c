/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_camera.c — Camera dispatcher.
 *
 * Routes public ml_camera_* calls through a vtable (ml_camera_ops_t)
 * stored in cam->ops at create time.  Adding a new backend only
 * requires implementing the ops struct and adding one case to
 * ml_camera_create.
 */

#include "io/ml_camera_private.h"

ml_camera_t *
ml_camera_create(const ml_camera_cfg_t *cfg, ml_pool_t *pool)
{
    if (!cfg || !pool)
        return NULL;

    switch (cfg->backend) {
#ifdef ML_CAMERA_HAS_V4L2
    case ML_CAMERA_BACKEND_V4L2: return ml_camera_v4l2_create(cfg, pool);
#endif
#ifdef ML_CAMERA_HAS_RTSP
    case ML_CAMERA_BACKEND_RTSP: return ml_camera_rtsp_create(cfg, pool);
#endif
#ifdef ML_CAMERA_HAS_FILE
    case ML_CAMERA_BACKEND_FILE: return ml_camera_file_create(cfg, pool);
#endif
    case ML_CAMERA_BACKEND_IMAGE:
        return ml_camera_stub_create(cfg, pool);
    default:
        return NULL;
    }
}

void ml_camera_destroy(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->destroy)
        return;
    cam->ops->destroy(cam);
}

int ml_camera_start(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->start)
        return -1;
    return cam->ops->start(cam);
}

int ml_camera_stop(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->stop)
        return -1;
    return cam->ops->stop(cam);
}

int ml_camera_read(ml_camera_t *cam, ml_frame_t *frame, int timeout_ms)
{
    if (!cam || !cam->ops || !cam->ops->read)
        return -1;
    return cam->ops->read(cam, frame, timeout_ms);
}

void ml_camera_frame_release(ml_camera_t *cam, ml_frame_t *frame)
{
    if (!cam || !cam->ops || !cam->ops->frame_release)
        return;
    cam->ops->frame_release(cam, frame);
}

int ml_camera_set_callback(ml_camera_t *cam, ml_camera_frame_cb cb, void *data)
{
    if (!cam || !cam->ops || !cam->ops->set_callback)
        return -1;
    return cam->ops->set_callback(cam, cb, data);
}

int ml_camera_get_width(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->get_width)
        return -1;
    return cam->ops->get_width(cam);
}

int ml_camera_get_height(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->get_height)
        return -1;
    return cam->ops->get_height(cam);
}

int ml_camera_get_fps(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->get_fps)
        return -1;
    return cam->ops->get_fps(cam);
}

int ml_camera_get_state(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->get_state)
        return -1;
    return cam->ops->get_state(cam);
}

const char *ml_camera_error(ml_camera_t *cam)
{
    if (!cam || !cam->ops || !cam->ops->error)
        return NULL;
    return cam->ops->error(cam);
}
