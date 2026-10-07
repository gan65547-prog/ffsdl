/*
 * Copyright (c) 2026 Gan
 * SPDX-License-Identifier: MIT
 *
 * ml_display SDL2 backend.  It presents RGB/BGR software frames in an SDL
 * texture.  On the target, set SDL_VIDEODRIVER=kmsdrm to place the window on
 * the HDMI/KMS display without an X11 or Wayland compositor.
 */

#include "platform/ml_display.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ml_display_s {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    int           display_w;
    int           display_h;
    int           frame_w;
    int           frame_h;
    uint32_t      frame_fmt;
    int           own_sdl;
    char          error[128];
    ml_pool_t    *pool;
    int           own_pool;
};

static void set_error(ml_display_t *d, const char *message)
{
    if (!d || !message)
        return;
    snprintf(d->error, sizeof(d->error), "%s", message);
}

ml_display_t *ml_display_create(ml_pool_t *pool)
{
    ml_display_t *d;
    int own_pool = 0;

    if (!pool) {
        pool = ml_alloc(sizeof(*pool));
        if (!pool)
            return NULL;
        if (ml_pool_create(pool, 0) != 0) {
            ml_free(pool);
            return NULL;
        }
        own_pool = 1;
    }

    d = pool->f_alloc(pool, sizeof(*d));
    if (!d) {
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    d->pool = pool;
    d->own_pool = own_pool;
    d->error[0] = '\0';
    return d;
}

void ml_display_destroy(ml_display_t *d)
{
    if (!d)
        return;
    if (d->texture)  SDL_DestroyTexture(d->texture);
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window)   SDL_DestroyWindow(d->window);
    if (d->own_sdl)  SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (d->own_pool) {
        ml_pool_destroy(d->pool);
        ml_free(d->pool);
    } else {
        d->pool->f_free(d->pool, d);
    }
}

int ml_display_init(ml_display_t *d, const ml_display_cfg_t *cfg)
{
    ml_display_cfg_t defaults = ML_DISPLAY_CFG_DEFAULT;
    Uint32 flags = SDL_WINDOW_RESIZABLE;
    const char *renderer_mode;
    int displays;

    if (!d)
        return -1;
    if (!cfg)
        cfg = &defaults;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            set_error(d, SDL_GetError());
            return -1;
        }
        d->own_sdl = 1;
    }

    displays = SDL_GetNumVideoDisplays();
    if (cfg->display_index < 0 || cfg->display_index >= displays) {
        set_error(d, "requested SDL display does not exist");
        return -1;
    }
    if (cfg->fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    d->display_w = cfg->preferred_w > 0 ? cfg->preferred_w : 1280;
    d->display_h = cfg->preferred_h > 0 ? cfg->preferred_h : 720;
    d->window = SDL_CreateWindow("clib video output",
                                 SDL_WINDOWPOS_CENTERED_DISPLAY(cfg->display_index),
                                 SDL_WINDOWPOS_CENTERED_DISPLAY(cfg->display_index),
                                 d->display_w, d->display_h, flags);
    if (!d->window) {
        set_error(d, SDL_GetError());
        return -1;
    }
    /* KMSDRM can use a framebuffer renderer when Mesa EGL/DRI is absent. */
    renderer_mode = getenv("ML_SDL_RENDERER");
    if (renderer_mode && strcmp(renderer_mode, "software") == 0) {
        d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_SOFTWARE);
    } else {
        d->renderer = SDL_CreateRenderer(d->window, -1,
                                         SDL_RENDERER_ACCELERATED |
                                         SDL_RENDERER_PRESENTVSYNC);
    }
    if (!d->renderer) {
        /* SDL's dummy driver and a few KMS configurations do not expose an
         * accelerated renderer.  The display abstraction still works with
         * the software renderer, which also makes this backend testable in
         * headless CI. */
        d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!d->renderer) {
        set_error(d, SDL_GetError());
        return -1;
    }
    SDL_GetRendererOutputSize(d->renderer, &d->display_w, &d->display_h);
    return 0;
}

static int ensure_texture(ml_display_t *d, const ml_frame_t *frame)
{
    Uint32 format;

    if (frame->format == ML_PIXFMT_BGR888)
        format = SDL_PIXELFORMAT_BGR24;
    else if (frame->format == ML_PIXFMT_RGB888)
        format = SDL_PIXELFORMAT_RGB24;
    else {
        set_error(d, "SDL backend accepts RGB888 or BGR888 frames only");
        return -1;
    }

    if (d->texture && d->frame_w == (int)frame->width &&
        d->frame_h == (int)frame->height && d->frame_fmt == frame->format)
        return 0;

    if (d->texture) {
        SDL_DestroyTexture(d->texture);
        d->texture = NULL;
    }
    d->texture = SDL_CreateTexture(d->renderer, format,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   (int)frame->width, (int)frame->height);
    if (!d->texture) {
        set_error(d, SDL_GetError());
        return -1;
    }
    d->frame_w = (int)frame->width;
    d->frame_h = (int)frame->height;
    d->frame_fmt = frame->format;
    return 0;
}

int ml_display_show_frame_ex(ml_display_t *d, const ml_frame_t *frame,
                             const ml_osd_rect_t *rects, int n_rects,
                             const ml_osd_line_t *lines, int n_lines,
                             const ml_osd_point_t *points, int n_points,
                             const ml_osd_text_t *texts, int n_texts)
{
    SDL_Event event;

    (void)rects; (void)n_rects; (void)lines; (void)n_lines;
    (void)points; (void)n_points; (void)texts; (void)n_texts;
    if (!d || !frame || !d->renderer || !frame->data[0])
        return -1;
    if (ensure_texture(d, frame) != 0)
        return -1;
    if (SDL_UpdateTexture(d->texture, NULL, frame->data[0],
                          (int)frame->stride[0]) != 0) {
        set_error(d, SDL_GetError());
        return -1;
    }
    SDL_RenderClear(d->renderer);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
    SDL_RenderPresent(d->renderer);
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return -1;
    }
    return 0;
}

int ml_display_show_frame(ml_display_t *d, const ml_frame_t *frame,
                          const ml_osd_rect_t *rects, int n_rects,
                          const ml_osd_text_t *texts, int n_texts)
{
    return ml_display_show_frame_ex(d, frame, rects, n_rects,
                                    NULL, 0, NULL, 0, texts, n_texts);
}

int ml_display_get_width(ml_display_t *d) { return d ? d->display_w : 0; }
int ml_display_get_height(ml_display_t *d) { return d ? d->display_h : 0; }
int ml_display_is_available(void) { return 1; }
const char *ml_display_error(ml_display_t *d)
{
    return d && d->error[0] ? d->error : "no display error";
}
