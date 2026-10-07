/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_frame — Unified frame data structure for multi-plane video frames.
 *
 * Supports DMA-BUF zero-copy paths (V4L2/RGA/MPP) and user-space memory
 * paths (RTSP/file source).  Reference-counted with release callback for
 * buffer-pool integration.
 */

#ifndef __ML_FRAME_H__
#define __ML_FRAME_H__

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

/* ── pixel formats ───────────────────────────────────────────────── */

typedef enum {
    ML_PIXFMT_BGR888  = 0,   /* OpenCV default, single-plane, HWC */
    ML_PIXFMT_RGB888  = 1,   /* single-plane, HWC                */
    ML_PIXFMT_NV12    = 2,   /* Y + UV two-plane, MPP native     */
    ML_PIXFMT_NV21    = 3,   /* Y + VU two-plane                 */
    ML_PIXFMT_YUYV    = 4,   /* single-plane packed YUV 4:2:2    */
    ML_PIXFMT_MJPEG   = 5,   /* compressed, not decoded          */
    ML_PIXFMT_GRAY8   = 6,   /* single-plane grayscale           */
    ML_PIXFMT_NV16    = 7,   /* Y + UV two-plane, 4:2:2, UV=H   */
    ML_PIXFMT_COUNT
} ml_pixfmt_t;

/* per-format meta-information */
typedef struct {
    int num_planes;        /* 1 = packed, 2 = semi-planar, 3 = full-planar */
    int bpp[3];            /* bits-per-pixel per plane      */
    int h_ss[3];           /* horizontal sub-sampling shift */
    int v_ss[3];           /* vertical sub-sampling shift   */
} ml_pixfmt_desc_t;

/* ── DMA-BUF / cache flags ───────────────────────────────────────── */

enum {
    ML_FRAME_FLAG_NONE       = 0,
    ML_FRAME_FLAG_OWN_FD     = (1 << 0),  /* frame owns fds, close on release */
    ML_FRAME_FLAG_READ_SYNC  = (1 << 1),  /* CPU read needs cache sync before */
    ML_FRAME_FLAG_WRITE_SYNC = (1 << 2),  /* CPU write needs cache sync after */
    ML_FRAME_FLAG_DMABUF     = (1 << 3),  /* data source is DMA-BUF          */
};

/* ── release callback ─────────────────────────────────────────────── */

typedef struct ml_frame_s ml_frame_t;

typedef void (*ml_frame_release_cb)(void *owner, ml_frame_t *f);

/* ── frame struct ─────────────────────────────────────────────────── */

struct ml_frame_s {
    /* data planes — up to 3 (packed=1, NV12=2, YUV420p=3) */
    int       fd[3];
    void     *data[3];
    size_t    size[3];
    uint32_t  stride[3];
    uint32_t  offset[3];

    /* image properties */
    uint32_t  width;
    uint32_t  height;
    uint32_t  format;       /* ml_pixfmt_t */
    uint32_t  num_planes;

    /* timestamps */
    uint64_t  timestamp;    /* us */
    uint64_t  seq;

    /* source identifier (multi-camera reserve) */
    uint8_t  source_id;     /* 0=cam0, 1=cam1, ..., 255=dryrun */
    uint8_t  source_type;   /* ml_camera_backend_t */

    /* lifecycle */
    uint32_t           flags;
    int                refcount;
    void              *owner;
    ml_frame_release_cb release;

    void *priv;              /* reserved */
};

/* ── pixel-format descriptor table ────────────────────────────────── */

static const ml_pixfmt_desc_t ml_pixfmt_desc_table[ML_PIXFMT_COUNT] = {
    /* BGR888  */ { 1, {24,  0,  0}, {1,0,0}, {1,0,0} },
    /* RGB888  */ { 1, {24,  0,  0}, {1,0,0}, {1,0,0} },
    /* NV12    */ { 2, { 8, 16,  0}, {1,2,0}, {1,2,0} },
    /* NV21    */ { 2, { 8, 16,  0}, {1,2,0}, {1,2,0} },
    /* YUYV    */ { 1, {16,  0,  0}, {1,0,0}, {1,0,0} },
    /* MJPEG   */ { 1, { 0,  0,  0}, {1,0,0}, {1,0,0} },
    /* GRAY8   */ { 1, { 8,  0,  0}, {1,0,0}, {1,0,0} },
    /* NV16    */ { 2, { 8, 16,  0}, {1,2,0}, {1,1,0} },
};

/* ── query helpers ────────────────────────────────────────────────── */

static inline const ml_pixfmt_desc_t *
ml_pixfmt_get_desc(ml_pixfmt_t fmt)
{
    if ((unsigned)fmt >= ML_PIXFMT_COUNT) {
        return NULL;
    }
    return &ml_pixfmt_desc_table[fmt];
}

static inline const char *
ml_pixfmt_name(ml_pixfmt_t fmt)
{
    static const char *names[ML_PIXFMT_COUNT] = {
        "BGR888", "RGB888", "NV12", "NV21", "YUYV", "MJPEG", "GRAY8", "NV16"
    };
    if ((unsigned)fmt >= ML_PIXFMT_COUNT) {
        return "UNKNOWN";
    }
    return names[fmt];
}

static inline int
ml_pixfmt_bpp_total(ml_pixfmt_t fmt)
{
    const ml_pixfmt_desc_t *d = ml_pixfmt_get_desc(fmt);
    if (!d) {
        return 0;
    }
    return d->bpp[0] + d->bpp[1] + d->bpp[2];
}

static inline size_t
ml_frame_total_size(const ml_frame_t *f)
{
    if (!f) {
        return 0;
    }
    return f->size[0] + f->size[1] + f->size[2];
}

/* ── constructors ─────────────────────────────────────────────────── */

static inline void
ml_frame_init(ml_frame_t *f)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->fd[0] = -1;
    f->fd[1] = -1;
    f->fd[2] = -1;
    f->refcount = 1;
}

/* from DMA-BUF fds (V4L2 dqbuf / RGA output) */
static inline void
ml_frame_from_dmabuf(ml_frame_t *f,
                     int fd_y, int fd_uv,
                     void *data_y, void *data_uv,
                     size_t size_y, size_t size_uv,
                     uint32_t w, uint32_t h,
                     uint32_t stride_y, uint32_t stride_uv,
                     ml_pixfmt_t fmt, uint64_t ts, uint64_t seq,
                     uint32_t flags,
                     void *owner, ml_frame_release_cb release)
{
    if (!f) {
        return;
    }
    ml_frame_init(f);

    f->fd[0]       = fd_y;
    f->fd[1]       = fd_uv;
    f->fd[2]       = -1;
    f->data[0]     = data_y;
    f->data[1]     = data_uv;
    f->data[2]     = NULL;
    f->size[0]     = size_y;
    f->size[1]     = size_uv;
    f->size[2]     = 0;
    f->stride[0]   = stride_y;
    f->stride[1]   = stride_uv;
    f->stride[2]   = 0;
    f->offset[0]   = 0;
    f->offset[1]   = 0;
    f->offset[2]   = 0;
    f->width       = w;
    f->height      = h;
    f->format      = fmt;
    f->num_planes  = 2;
    f->timestamp   = ts;
    f->seq         = seq;
    f->flags       = flags;
    f->owner       = owner;
    f->release     = release;
}

/* from user-space memory (RTSP/file source) — single-plane packed */
static inline void
ml_frame_from_mem(ml_frame_t *f,
                  const void *src, size_t size,
                  uint32_t w, uint32_t h,
                  ml_pixfmt_t fmt, uint64_t ts, uint64_t seq)
{
    if (!f) {
        return;
    }
    ml_frame_init(f);

    f->data[0]     = (void *)src;
    f->data[1]     = NULL;
    f->data[2]     = NULL;
    f->size[0]     = size;
    f->size[1]     = 0;
    f->size[2]     = 0;
    f->stride[0]   = w * (ml_pixfmt_desc_table[fmt].bpp[0] / 8);
    f->stride[1]   = 0;
    f->stride[2]   = 0;
    f->offset[0]   = 0;
    f->offset[1]   = 0;
    f->offset[2]   = 0;
    f->width       = w;
    f->height      = h;
    f->format      = fmt;
    f->num_planes  = 1;
    f->timestamp   = ts;
    f->seq         = seq;
    f->flags       = ML_FRAME_FLAG_NONE;
    f->owner       = NULL;
    f->release     = NULL;
}

/* ── reference counting ───────────────────────────────────────────── */

static inline void
ml_frame_ref(ml_frame_t *f)
{
    if (f) {
        __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED);
    }
}

/* unref — on last reference, call release callback and close owned fds.
 *
 * Ownership contract: the release callback may free the ml_frame_t (e.g.
 * pool-allocated or dma-heap frames).  Callers must cache any frame fields
 * they need after the release callback runs.  This function does so for
 * flags and fd[] before dispatching to the callback. */
static inline void
ml_frame_unref(ml_frame_t *f)
{
    int i;

    if (!f) {
        return;
    }
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_RELAXED) > 0) {
        return;
    }

    /* Cache before release — callback may free f. */
    int flags = f->flags;
    int fd[3];
    for (i = 0; i < 3; i++) {
        fd[i] = f->fd[i];
    }

    if (f->release) {
        f->release(f->owner, f);
        /* f may be freed — do not access f after this point */
    }

    if (flags & ML_FRAME_FLAG_OWN_FD) {
        int seen[3] = {-1, -1, -1};
        int n_seen = 0, j;
        for (i = 0; i < 3; i++) {
            if (fd[i] < 0) {
                continue;
            }
            for (j = 0; j < n_seen; j++)
                if (seen[j] == fd[i]) {
                    break;
                }
            if (j == n_seen) {
                seen[n_seen++] = fd[i];
                close(fd[i]);
            }
        }
    }
}

/* ── deep copy (fallback when DMA-BUF unavailable) ────────────────── */

typedef struct ml_pool_s ml_pool_t;

int ml_frame_deep_copy(ml_frame_t *dst, const ml_frame_t *src, ml_pool_t *pool);

#endif /* __ML_FRAME_H__ */
