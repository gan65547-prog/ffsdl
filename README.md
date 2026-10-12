# ffsdl：Rockchip 板端 RTSP 硬解码与 HDMI 播放示例

`ffsdl` 是面向 Rockchip ARM64 Linux 板卡的 RTSP 播放示例：FFmpeg 负责拉流和解复用，
程序优先调用 Rockchip MPP 进行硬件解码，再通过 RGA 转换图像格式，最后由 SDL2 输出到
HDMI。让网络视频流从 RTSP 地址直达板端屏幕。

项目支持 H.264/H.265 RTSP 流，并提供 FFmpeg 4.3 和 6.1 两种 ABI 配置。即使 FFmpeg
自身没有集成 RKMPP 解码器，也可以使用项目的直接 MPP 解码路径。仓库还包含精简的 RTSP、
帧池、MPP/RGA 与 SDL2 适配代码，以及构建、运行和板端部署说明，适合验证 Rockchip 上的
硬解码与 KMS/DRM 显示链路，或作为 C 语言实现参考。

项目已在 108（aarch64）板端完成 FFmpeg 4.3 构建、dummy 显示测试和真实 RTSP 拉流测试；
FFmpeg 6.1 配置也通过了本机编译和 CTest 兼容性检查。具体环境要求、构建命令和运行方法
见下文。

## 项目结构

- `src/clib_rtsp_sdl_player.c`：播放器入口。
- `clib/`：本示例使用的 RTSP、帧池、Rockchip MPP/RGA 与 SDL2 适配代码。
- `include/ffsdl_ffmpeg43.h`：FFmpeg ABI 兼容处理。
- `tests/`：RGA 与 SDL 显示相关检查程序。

## 开源许可

本项目包含第三方组件及其头文件、运行库。开源或再分发前，请查看各组件随附的许可证，
并遵守相应的版权声明和再分发条件。`clib/` 的许可证见 [`clib/LICENSE`](clib/LICENSE)。

入口是 `src/clib_rtsp_sdl_player.c`。`clib/` 是仅包含本 demo 所需 RTSP、帧池、
RKMPP/RGA 与 SDL2 适配代码的最小提取。FFmpeg 4.3 起的 ABI 要求由项目的
`include/ffsdl_ffmpeg43.h` 管理（当前 SDK 为 FFmpeg 6.1，名称保留以兼容现有源码）。

## FFmpeg 版本与切换

项目支持两个独立的 FFmpeg SDK，必须保证头文件、链接库和运行时 `.so` 来自同一版本，
不能混用 FFmpeg 4.3 和 6.1：

| 选项 | SDK 目录 | 主要 ABI | 用途 |
|---|---|---|---|
| `4.3` | `third_party/ffmpeg-4.3` | `libavcodec.so.58`、`libavformat.so.58` | 兼容旧板端环境 |
| `6.1` | `third_party/ffmpeg-6.1` | `libavcodec.so.60`、`libavformat.so.60` | 新版板端/RKMPP 环境 |

代码使用 C11、POSIX、SDL2、DRM/GBM、Rockchip MPP 和 RGA 接口。Linux 板端优先使用
直接 MPP 解码；MPP 不可用时才回退到 FFmpeg 的 `h264_rkmpp`/`hevc_rkmpp` 或软件解码。

注意：当前方案不依赖 FFmpeg 4.3 的 `h264_rkmpp` 补丁包。FFmpeg 4.3 负责 RTSP
解复用，新增的 `ml_mpp_decoder.c` 直接调用板端 MPP 完成硬件解码。请用以下命令检查
FFmpeg 自身是否包含 RKMPP：

```bash
ffmpeg -hide_banner -decoders | grep rkmpp
```

看到 `h264_rkmpp` 或 `hevc_rkmpp` 表示 FFmpeg 自身支持对应 RKMPP 解码器；当前 Linux
板端即使没有该 FFmpeg 解码器，也可以使用项目的直接 MPP 路径。

`FFMPEG_VERSION` 只接受 `4.3` 或 `6.1`，每个版本建议使用独立的 build 目录：

```bash
cmake -S event -B build-43 -DFFMPEG_VERSION=4.3 -DBUILD_TESTING=ON
cmake --build build-43 -j
ctest --test-dir build-43 --output-on-failure

cmake -S event -B build-61 -DFFMPEG_VERSION=6.1 -DBUILD_TESTING=ON
cmake --build build-61 -j
ctest --test-dir build-61 --output-on-failure

# 108 板端当前 FFmpeg 4.3 配置的最终编译命令
cmake --build build-board-43-rkmpp --target ffsdl
```

切换 SDK 后如果复用了同一个 `clib` 构建目录，必须先清理旧对象，避免旧版本头文件
残留导致 ABI 混用：

```bash
make -C event/clib clean
rm -rf build-43 build-61
```

不要把 `third_party/ffmpeg-4.3/include` 和 `third_party/ffmpeg-6.1/include`
同时放在编译器的 include 搜索路径中；头文件、链接库和运行时库必须来自同一版本。

## 内存池限制

clib 内存池使用普通单链表保存释放后的空闲块，适合本程序的单线程调用。
它不是通用的线程安全分配器，使用时必须遵守以下边界：

1. 同一个 `ml_pool_t` 不能被多个线程并发调用；如需共享，调用方必须在外部加锁。
2. 只能释放由同一个内存池 `f_alloc` 返回、且尚未释放的指针。
3. 不检查非法指针、跨池释放、内存越界和释放后继续使用；这些属于调用方责任。
4. 重复释放同一个合法指针不会重复加入空闲链表，但仍属于错误用法，不能依赖这个行为。
5. 空闲块采用单链表线性查找，不做拆分和合并；空闲块很多时，查找时间会增加，内存碎片
   由内存池页大小和申请模式决定。
6. `ml_pool_destroy` 只能在所有池内对象不再使用后调用；销毁后不能继续访问池内对象。

这些限制属于内存池接口的使用约束。如果需要多线程共享、非法释放检测或高并发分配，
应在应用层增加同步和检查，或扩展内存池实现。

## 构建环境

项目默认使用当前目录下的 `third_party/` 运行库。注意：`.so` 运行库不能替代编译
所需的头文件和开发链接文件。编译时必须使用系统开发包，或准备如下 SDK 目录：

```text
third_party/
├── ffmpeg-4.3/include/libavformat/avformat.h
├── ffmpeg-4.3/lib/libavformat.so.58
├── ffmpeg-6.1/include/libavformat/avformat.h
├── ffmpeg-6.1/lib/libavformat.so.60
├── sdl2/include/SDL2/SDL.h
└── lib/aarch64-linux-gnu/
```

项目默认将 `FFMPEG_VERSION` 设置为 `4.3`，并自动使用
`third_party/ffmpeg-4.3`；SDL2 默认使用 `third_party/sdl2`。

Ubuntu 构建机安装基础开发依赖：

```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential pkg-config \
  libsdl2-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libdrm-dev libgbm-dev libegl1-mesa-dev
```

FFmpeg 6.1 的头文件和开发库应安装到系统标准路径，或通过 `FFMPEG_ROOT` 指定 SDK。

```bash
cmake -S event -B build-61 -DFFMPEG_VERSION=6.1
cmake --build build-61 -j
```

FFmpeg 4.3 与 6.1 可按命令切换，SDK 目录分别为
`third_party/ffmpeg-4.3` 和 `third_party/ffmpeg-6.1`：

```bash
cmake -S event -B build-43 -DFFMPEG_VERSION=4.3
cmake --build build-43 -j

cmake -S event -B build-61 -DFFMPEG_VERSION=6.1
cmake --build build-61 -j
```

如果 SDK 不在项目目录，可显式指定对应版本的路径：

```bash
cmake -S event -B build-61 \
  -DFFMPEG_VERSION=6.1 \
  -DFFMPEG_ROOT=/opt/ffmpeg-6.1 \
  -DSDL2_ROOT=/opt/sdl2
cmake --build build-61 -j
```

使用独立的 FFmpeg 或 SDL2 SDK 时：

```bash
cmake -S event -B build-43 \
  -DFFMPEG_VERSION=4.3 \
  -DFFMPEG_ROOT=/path/to/ffmpeg-4.3 \
  -DSDL2_ROOT=/path/to/sdl2
cmake --build build-43 -j
```

## 板端运行

项目的 `third_party/lib/aarch64-linux-gnu` 包含 ARM64 运行所需的共享库，包括
SDL2、RGA、Rockchip MPP、DRM/GBM/EGL、Rockchip Mesa DRI；选定版本的 FFmpeg
运行库位于对应的 `third_party/ffmpeg-4.3/lib` 或 `third_party/ffmpeg-6.1/lib`，
以及其非 glibc 传递依赖。该目录只能用于 ARM64 Linux，不能在 macOS 或其他架构上直接使用。

HDMI/KMS 输出依赖以下运行库和设备权限：`libsdl2-2.0-0`、`libdrm2`、
`libgbm1`、`libegl1`、`libegl-mesa0`、`libgl1-mesa-dri`、
`mesa-vulkan-drivers`，以及内核提供的 `/dev/dri/card0`。Rockchip Mesa 包还应
提供 `/usr/lib/aarch64-linux-gnu/dri/rockchip_dri.so`。Ubuntu 22.04/24.04 板端安装命令：

```bash
sudo apt-get update
sudo apt-get install -y \
  libsdl2-2.0-0 libdrm2 libgbm1 libegl1 libegl-mesa0 \
  libgl1-mesa-dri mesa-vulkan-drivers
```

运行用户需要属于 `video` 和 `render` 组。`SDL_VIDEODRIVER=kmsdrm` 要求从板端
本地 tty/虚拟终端运行；无 tty 的 SSH 会话可能报 `kmsdrm not available`。

### 部署方式

在 ARM64 编译机或 108 板端编译完成后，将同一版本的可执行文件、clib 动态库和 SDK 运行库复制到板端。
以下以 FFmpeg 4.3 为例：

```bash
scp build-board-43-rkmpp/ffsdl build-board-43-rkmpp/test_clib_sdl_display clib/libclib.so \
  gan@<板端IP>:/tmp/ffsdl-runtime/
scp -r third_party/lib third_party/ffmpeg-4.3/lib \
  gan@<板端IP>:/tmp/ffsdl-runtime/third_party/
```

也可以直接把整个项目的 `third_party/` 目录随程序一起部署。板端最终目录至少应包含：

```text
/tmp/ffsdl-runtime/ffsdl
/tmp/ffsdl-runtime/libclib.so
/tmp/ffsdl-runtime/test_clib_sdl_display
/tmp/ffsdl-runtime/third_party/lib/aarch64-linux-gnu/
/tmp/ffsdl-runtime/third_party/ffmpeg-4.3/lib/
```

### 运行方式

登录板端后进入部署目录。将 `ffsdl` 与 `libclib.so` 放在同一目录：

```bash
export LIBGL_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
export EGL_PLATFORM=gbm
export LD_LIBRARY_PATH=$PWD/third_party/ffmpeg-4.3/lib:$PWD/third_party/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:$PWD:/opt/mlfw-dev/mlfw/lib

# SDL2 显示接口测试（无显示器）
SDL_VIDEODRIVER=dummy ./test_clib_sdl_display

# HDMI/KMS 输出
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 ./ffsdl \
  'rtsp://<host>:8554/<path>' 0
```

使用 FFmpeg 4.3 时，把上面的 `ffmpeg-6.1/lib` 替换为
`ffmpeg-4.3/lib`，并且必须使用由 `-DFFMPEG_VERSION=4.3` 生成的可执行文件和
`libclib.so`。不要把两个版本的 FFmpeg `.so` 放在同一个运行时目录中。

### 108 板端检查清单

```bash
uname -m                         # 应为 aarch64
id                               # 用户应在 video、render 组
ls -l /dev/dri/card0             # KMS 设备
SDL_VIDEODRIVER=dummy ./test_clib_sdl_display
```

真实 HDMI 测试必须在本地 tty/虚拟终端执行：

```bash
export SDL_VIDEODRIVER=kmsdrm
export SDL_KMSDRM_DEVICE_INDEX=0
./ffsdl 'rtsp://<rtsp-host>:8554/<stream-path>' 0
```

如果需要强制关闭 RKMPP 进行对照测试，可设置：

```bash
export FFSDL_DISABLE_RKMPP=1
```

未设置该变量时程序会优先尝试直接 MPP，失败后再尝试 FFmpeg RKMPP 或软件解码。

### 当前验收记录

在 108（aarch64）上，FFmpeg 4.3 版本已通过构建、dummy 显示测试和真实 RTSP 拉流测试，
输出 `decoder=mpp_direct mode=hardware stream=1280x720`，连续 10 秒输出超过 100 帧。
FFmpeg 6.1 已通过本机配置、编译和 CTest 兼容性检查。

例如使用 RTSP 测试流：

```bash
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 ./ffsdl \
  'rtsp://<rtsp-host>:8554/<stream-path>' 0
```

参数说明：第一个参数是 RTSP 地址，第二个可选参数是 SDL 显示设备索引，通常使用
`0`。退出程序使用 `Ctrl+C`。

程序支持 1280x720、25 fps 等常见 H.264 RTSP 流；当 FFmpeg 构建包含 RKMPP 时，
通过 `h264_rkmpp`/`hevc_rkmpp` 进行硬件解码，否则使用软件解码。随后由 RGA
完成 BGR888 转换并由 SDL2 输出到 HDMI。HDMI 输出需要板端可用的
KMSDRM/EGL/DRI 图形运行环境。
