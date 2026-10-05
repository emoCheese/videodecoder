# VideoDecoder

RTSP / RTP / 本地文件的视频拉流播放器：**FFmpeg 拉流 → 软解 → NV12 → OpenGL(videowidget) 上屏**，界面用 Qt6 Widgets。
C++20，**简单可靠优先**（带锁环形队列、RAII、`std::jthread` + `std::stop_token`）。

## 1. 数据流与线程模型

```
rtsp:// | rtp://(SDP 文件) | 本地文件
   │  av_read_frame（可被 interrupt_callback 打断）        Demuxer 线程
   ▼
RingBuffer<avpacket::Packet, 30, BlockingFullPolicy>        ← 满则反压拉流端，视频包不覆盖丢弃（防花屏）
   │  pop                                                   Decoder 线程
   ▼
avcodec_send_packet / avcodec_receive_frame
   │  sws_getCachedContext → AV_PIX_FMT_NV12（偶数宽高，自动适配中途换分辨率）
   ▼
avframe::VideoFramePtr { AVFrameHolder, ptsSeconds, recvAt, decodedAt }
   │  解码线程回调 → 只写"最新帧槽"（mutex + 覆盖旧帧）
   ▼
GUI QTimer(16ms) 取最新帧 → 适配成 Nv12FrameView + FrameLease
   ▼
VideoWidget::onFrameReady → paintGL：上传 NV12 双纹理 → YUV→RGB 着色器（宽高比 letterbox）
```

* 停止顺序由 Pipeline 唯一决定：`queue.close()`（先解阻塞，否则 demuxer 阻塞在满队列 push 上会 join 死锁）→ `demuxer.stop()` → `decoder.stop()`。
* `Pipeline::isRunning()` 只在**解码线程彻底收工**后转 false：上游上报"流已结束"时解码线程可能还在回调残留帧，过早收尾会截断画面。
* 帧投递用"定时器拉取 + 单槽覆盖"而非跨线程信号：不产生事件洪水、天然只保留最新帧，代价 ≤16ms 调度延迟。

## 2. 模块边界与职责

| 模块 | 唯一职责 | 明确不做 | 可依赖 | 线程 | 拥有 |
|---|---|---|---|---|---|
| `Decoder/RingBuffer.hpp` | 通用带锁环形队列（容量/满策略/唤醒/关闭） | 不认识包与帧，无业务语义 | STL | 无 | 缓冲槽位 |
| `Decoder/PacketData.h` | FFmpeg RAII 与跨线程 DTO | 无逻辑、无策略 | FFmpeg | 无 | RAII 所有权 |
| `Decoder/Demuxer.{h,cpp}` | URL → 编码包序列（探测、超时、中断、只推视频包） | 不解码、不知 NV12、不 close 队列 | RingBuffer/PacketData/FFmpeg | 自有 1 线程 | `AVFormatContext` |
| `Decoder/Decoder.{h,cpp}` | 编码包 → NV12 帧（解码上下文、格式转换、pts） | 不拉流、不管网络、不拥有队列 | RingBuffer/PacketData/FFmpeg | 自有 1 线程 | `AVCodecContext`/`SwsContext` |
| `Decoder/Pipeline.{h,cpp}` | 装配与生命周期编排（队列归属、启停顺序、回调转发），pimpl 隐藏内部 | 不实现拉流/解码细节 | Demuxer/Decoder | 编排者 | 包队列（唯一 `close/reset` 处） |
| `VideoWidget/*` | NV12 视图 → GPU 上屏（GL 资源、着色器、宽高比、上屏帧率） | 不知帧从哪来、**不依赖 FFmpeg/Pipeline** | Qt + `nv12frameview.h` | 仅 GUI 线程 | GL 纹理/VAO/VBO、当前帧保活句柄 |
| `mainwindow.{h,cpp}` | 交互 + 跨线程投递 + 状态展示 + 视图适配器 | 不改帧内容、不做拉流/解码 | Pipeline/VideoWidget | 仅 GUI 线程（回调只写单槽） | `Pipeline`、UI 对象 |

**边界靠构建系统强制**：`VideoWidget` 是独立目标，不链接 `Decoder`、也不带它的 include 目录，因此控件物理上无法包含 FFmpeg/Pipeline 头；
`ringbuffer_test` 只链接 `DecoderPrimitives`（纯 header 接口目标），不链接 FFmpeg。
公共头只有 `Decoder/Pipeline.h` 与 `Decoder/PacketData.h`，其余标注为内部头文件。

## 3. 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-20
cmake --build build -j
```
依赖：Qt 6.5+（Core/Widgets/OpenGL/OpenGLWidgets）、FFmpeg 7（libavcodec/libavformat/libavutil/libswscale）、CMake 3.19+、支持 C++20 的编译器。
路径在 `CMakeLists.txt` / `Decoder/CMakeLists.txt` 顶部（Qt 6.10.3 与 FFmpeg 7 安装位置）。要求 `-Wall -Wextra -Wpedantic` 零告警。

## 4. 运行

```bash
./build/VideoDecoder                                   # 界面里填地址后点"连接"
./build/VideoDecoder rtsp://192.168.1.10:554/stream     # 或命令行直接给地址
./build/VideoDecoder video.mp4                          # 本地文件
./build/VideoDecoder stream.sdp                         # RTP（SDP 文件）
```
界面：顶部 URL 输入框 + 连接/断开，状态栏显示 **分辨率 / 上屏帧率与丢帧 / 端到端延迟(收包→上屏)**；断开时画面清空为黑屏。

## 5. 测试与验收

```bash
ctest --test-dir build --output-on-failure
```
* `ringbuffer`：FIFO、覆盖策略、阻塞 push/pop、`close()` 唤醒、关闭后仍可取残留元素、`reset()` 复用、4 生产者/2 消费者无丢失无重复。
* `pipeline_smoke_file`：自动生成素材（`rawvideo + /dev/zero`，本机 FFmpeg 未编 avdevice 故不用 lavfi）后解码校验。
* 手动：`./build/tests/pipeline_smoke <地址> [--frames N]` 校验 NV12/偶数尺寸/停机 <500ms。

**RTSP 端到端**（mediamtx + ffmpeg 推流 + 本程序拉流）：

推流端常驻在 `~/tools/mediamtx/`（自带配置与 `test/push_rtsp.sh`，默认推本机测试视频）：
```bash
~/tools/mediamtx/test/push_rtsp.sh                      # 默认视频 → rtsp://127.0.0.1:8554/live
~/tools/mediamtx/test/push_rtsp.sh 其它视频.mp4 rtsp://127.0.0.1:8554/live
~/tools/mediamtx/test/push_rtsp.sh --restart            # 先杀掉占用 8554 的进程再重启服务端
RTSP_PORT=8557 ~/tools/mediamtx/test/push_rtsp.sh       # 换端口（会同步给 mediamtx；其它服务端口可用 MTX_* 覆盖）
```
该脚本的行为：端口已占用则复用已运行的 mediamtx，端口空闲则按 `mediamtx.yml` 自行启动；日志写在 `test/logs`（不可写时自动降级到临时目录）；Ctrl-C 结束时若服务端是它启动的会一并回收。
拉流验收（该脚本自动起服务端 + 推流 + 校验；mediamtx 默认取 `~/tools/mediamtx`）：
```bash
tests/run_rtsp_test.sh "/home/fox/视频/你的视频.mp4" 120
```
实测（本项目开发机，1280x720 h264 High + AAC）：`帧数=124 坏帧=0 分辨率=1280x720 停机=26ms`；ASan 构建同样跑通（`帧数=64 坏帧=0 停机=22ms`，无 ASan 报错）。
注意：**mediamtx 必须带配置文件启动**（`mediamtx mediamtx.yml`），无配置时"路径未配置"会直接以 `400 Bad Request` 拒绝推流——两个脚本都已处理。

**RTP/SDP 端到端**（无服务端依赖）：
```bash
cd build && FF=/home/fox/workspace/FFmpeg/install/ffmpeg7-shared/bin/ffmpeg
$FF -re -stream_loop -1 -i tests/testmedia/vd_test.mp4 -c copy -f rtp rtp://127.0.0.1:5004 -sdp_file rtp.sdp &
./tests/pipeline_smoke rtp.sdp --frames 60
```
读 SDP 依赖 `protocol_whitelist`（已在 `Demuxer::applyInputOptions` 中放行 rtp/udp）。

## 6. Sanitizer（显式 CMake 开关）

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-20 \
      -DVD_SANITIZE_ADDRESS=ON -DVD_SANITIZE_UNDEFINED=ON && cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure      # 环境变量由 tests/CMakeLists.txt 自动注入

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-20 \
      -DVD_SANITIZE_THREAD=ON && cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```
* ASan 与 TSan 互斥，CMake 会直接报错要求分开的构建目录。
* `tests/sanitizer/lsan.supp` 过滤第三方（FFmpeg/Qt/X11）一次性分配；`tests/sanitizer/tsan.supp` 过滤未插桩的 Qt/FFmpeg/dbus/glib/fontconfig 噪声——本项目源码零报告（GUI 运行时如出现涉及 `Decoder/`、`VideoWidget/`、`mainwindow.cpp` 的报告，请先删掉对应抑制行再复现）。

## 7. 已知限制（本轮非目标）

音频解码/播放与音视频同步、PTS 时钟同步显示、硬件解码、包池/帧池、自动重连、截图/全屏/录制、GL 不可用时的光栅降级渲染、NV12 直通零拷贝优化、无锁队列（`Decoder/readerwriterqueue-1.0.7/` 保留未接入）。
显示需要可用的 OpenGL 3.3 Core 环境：上下文创建失败时程序会在日志中给出可执行诊断（`QT_XCB_GL_INTEGRATION=xcb_egl`、`LIBGL_ALWAYS_SOFTWARE=1`），但不会回退到软件绘制。
