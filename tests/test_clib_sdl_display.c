#include "memory/ml_frame.h"
#include "memory/ml_pool.h"
#include "platform/ml_display.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    ml_pool_t pool;
    ml_display_cfg_t cfg = ML_DISPLAY_CFG_DEFAULT;
    ml_display_t *display;
    ml_frame_t frame;
    uint8_t pixels[4 * 4 * 3];

    memset(pixels, 0x80, sizeof(pixels));
    ml_pool_create(&pool, 0);
    display = ml_display_create(&pool);
    if (!display) {
        fprintf(stderr, "display create failed\n");
        ml_pool_destroy(&pool);
        return 1;
    }
    cfg.fullscreen = 0;
    cfg.preferred_w = 4;
    cfg.preferred_h = 4;
    if (ml_display_init(display, &cfg) != 0) {
        fprintf(stderr, "display init failed: %s\n", ml_display_error(display));
        ml_display_destroy(display);
        ml_pool_destroy(&pool);
        return 1;
    }

    ml_frame_from_mem(&frame, pixels, sizeof(pixels), 4, 4,
                      ML_PIXFMT_BGR888, 0, 0);
    if (ml_display_show_frame(display, &frame, NULL, 0, NULL, 0) != 0) {
        fprintf(stderr, "frame display failed: %s\n", ml_display_error(display));
        ml_display_destroy(display);
        ml_pool_destroy(&pool);
        return 1;
    }

    ml_display_destroy(display);
    ml_pool_destroy(&pool);
    return 0;
}
