/* Hardware regression test: MPP DMA-BUF NV12 -> RGA DMA-BUF BGR. */
#include "platform/ml_rga.h"
#include "memory/ml_pool.h"

#ifdef __linux__
#include <mpp_buffer.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const int w = 64, h = 64;
    const size_t nv12_size = (size_t)w * h * 3 / 2;
    const size_t bgr_size = (size_t)w * h * 3;
    MppBufferGroup group = NULL;
    MppBuffer src = NULL, dst = NULL;
    ml_rga_t *rga = NULL;
    ml_pool_t pool = {0};
    unsigned char *in, *out;
    const char *stage = "allocate DMA-BUFs";
    int rc = 1;

    if (ml_pool_create(&pool, 4096) != 0)
        goto done;
    if (mpp_buffer_group_get_internal(&group,
                                      MPP_BUFFER_TYPE_DMA_HEAP |
                                      MPP_BUFFER_FLAGS_CONTIG) != MPP_OK ||
        mpp_buffer_get(group, &src, nv12_size) != MPP_OK ||
        mpp_buffer_get(group, &dst, bgr_size) != MPP_OK)
        goto done;
    in = mpp_buffer_get_ptr(src);
    out = mpp_buffer_get_ptr(dst);
    if (!in || !out || mpp_buffer_get_fd(src) < 0 || mpp_buffer_get_fd(dst) < 0)
        goto done;

    /* Neutral full-range NV12 must produce a neutral BGR image. */
    stage = "CPU write sync";
    if (mpp_buffer_sync_begin(src) != MPP_OK)
        goto done;
    memset(in, 128, nv12_size);
    if (mpp_buffer_sync_end(src) != MPP_OK)
        goto done;
    stage = "create RGA";
    rga = ml_rga_create(&pool);
    stage = "RGA DMA-BUF CSC";
    if (!rga || ml_rga_nv12_to_bgr_dmabuf(rga, mpp_buffer_get_fd(src), w, h,
                                           w, h, mpp_buffer_get_fd(dst), w, h))
        goto done;
    stage = "CPU read sync";
    if (mpp_buffer_sync_ro_begin(dst) != MPP_OK)
        goto done;
    if (out[0] < 120 || out[0] > 136 || out[1] < 120 || out[1] > 136 ||
        out[2] < 120 || out[2] > 136) {
        fprintf(stderr, "unexpected BGR: %u,%u,%u\n", out[0], out[1], out[2]);
        (void)mpp_buffer_sync_ro_end(dst);
        goto done;
    }
    (void)mpp_buffer_sync_ro_end(dst);
    rc = 0;
done:
    if (rc != 0)
        fprintf(stderr, "RGA DMA-BUF CSC failed at %s%s%s\n", stage,
                rga && ml_rga_error(rga) ? ": " : "",
                rga && ml_rga_error(rga) ? ml_rga_error(rga) : "");
    if (rga) ml_rga_destroy(rga);
    if (src) mpp_buffer_put(src);
    if (dst) mpp_buffer_put(dst);
    if (group) mpp_buffer_group_put(group);
    ml_pool_destroy(&pool);
    return rc;
}
#else
int main(void) { return 0; }
#endif
