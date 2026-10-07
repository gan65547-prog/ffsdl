# ARM64 第三方运行库

`lib/aarch64-linux-gnu` 是当前 demo 在 ARM64 板端运行所需的共享库集合，包含：

- 客户 FFmpeg 4.3 ABI 系列库：`libavcodec.so.58`、`libavformat.so.58`、
  `libavutil.so.56`、`libswscale.so.5`、`libswresample.so.3`
- SDL2
- Rockchip MPP 和 RGA
- DRM、GBM、EGL、Mesa 以及 `dri/rockchip_dri.so`
- FFmpeg 和 SDL2 所需的非 glibc 传递依赖

这些库是从 build100 客户 FFmpeg 运行库和 108 板端运行环境整理得到的，目标为
Ubuntu 24.04 ARM64。它们不能直接用于 macOS 或其他架构。

板端运行前设置：

```bash
export LIBGL_DRIVERS_PATH="$PWD/third_party/lib/aarch64-linux-gnu/dri"
export EGL_PLATFORM=gbm
export LD_LIBRARY_PATH="$PWD/third_party/lib/aarch64-linux-gnu:$PWD"
```

FFmpeg 4.3 ABI 库当前未编入 `h264_rkmpp` / `hevc_rkmpp` 时，demo 会回退到软件
解码。要使用 RKMpp 硬解，需要替换为带 Rockchip MPP 解码器的同 ABI FFmpeg 库。
