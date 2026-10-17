#include "io/ml_camera.h"
#include "memory/ml_frame.h"
#include "memory/ml_pool.h"
#include "platform/ml_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s <rtsp-url> [display-index]\n"
            "Set SDL_VIDEODRIVER=kmsdrm for direct HDMI/KMS output.\n",
            program);
}

int main(int argc, char **argv)
{
    ml_pool_t pool;
    ml_camera_cfg_t camera_cfg = ML_CAMERA_CFG_DEFAULT;
    ml_display_cfg_t display_cfg = ML_DISPLAY_CFG_DEFAULT;
    ml_camera_t *camera;
    ml_display_t *display;
    unsigned long frame_count = 0;
    unsigned int read_failures = 0;

    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 2;
    }

    ml_pool_create(&pool, 0);
    camera_cfg.backend = ML_CAMERA_BACKEND_RTSP;
    camera_cfg.device = argv[1];
    if (argc == 3)
        display_cfg.display_index = atoi(argv[2]);

    camera = ml_camera_create(&camera_cfg, &pool);
    if (!camera) {
        fprintf(stderr, "RTSP open failed\n");
        ml_pool_destroy(&pool);
        return 1;
    }
    display = ml_display_create(&pool);
    if (!display || ml_display_init(display, &display_cfg) != 0) {
        fprintf(stderr, "SDL display init failed: %s\n",
                display ? ml_display_error(display) : "out of memory");
        ml_camera_destroy(camera);
        ml_pool_destroy(&pool);
        return 1;
    }
    if (ml_camera_start(camera) != 0) {
        fprintf(stderr, "RTSP start failed: %s\n", ml_camera_error(camera));
        ml_display_destroy(display);
        ml_camera_destroy(camera);
        ml_pool_destroy(&pool);
        return 1;
    }

    for (;;) {
        ml_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        if (ml_camera_read(camera, &frame, camera_cfg.timeout_ms) != 0) {
            read_failures++;
            fprintf(stderr, "[ffsdl] RTSP read failed (%u), reconnecting: %s\n",
                    read_failures, ml_camera_error(camera));
            if (read_failures < 20) {
                usleep(100000);
                continue;
            }
            fprintf(stderr, "[ffsdl] RTSP read failed 20 consecutive times; stopping\n");
            break;
        }
        read_failures = 0;
        if (ml_display_show_frame(display, &frame, NULL, 0, NULL, 0) != 0) {
            fprintf(stderr, "SDL display stopped: %s\n", ml_display_error(display));
            ml_camera_frame_release(camera, &frame);
            break;
        }
        frame_count++;
        if (frame_count == 1 || frame_count % 100 == 0) {
            fprintf(stderr, "[ffsdl] frames=%lu output=%ux%u %s dmabuf=%s\n",
                    frame_count, frame.width, frame.height,
                    ml_pixfmt_name((ml_pixfmt_t)frame.format),
                    (frame.flags & ML_FRAME_FLAG_DMABUF) ? "yes" : "no");
        }
        ml_camera_frame_release(camera, &frame);
    }

    ml_camera_stop(camera);
    ml_display_destroy(display);
    ml_camera_destroy(camera);
    ml_pool_destroy(&pool);
    return 0;
}
