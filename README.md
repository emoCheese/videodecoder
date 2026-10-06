# VideoDecoder

RTSP / RTP / 本地文件的视频拉流播放器：**FFmpeg 拉流 → 硬解优先（NVIDIA `*_cuvid`，不可用自动回退软解）→ NV12 → OpenGL(videowidget) 上屏**，界面用 Qt6 Widgets。
C++20，**简单可靠优先**（带锁环形队列、RAII、`std::jthread` + `std::stop_token`）。

## 1. 数据流与线程模型

```
rtsp:// | rtp://(SDP 文件) | 本地文件
   │  av_read_frame（可被 interrupt_callback 打断）        Demuxer 线程
   ▼
RingBuffer<avpacket::Packet, 30, BlockingFullPolicy>        ← 满则反压拉流端，视频包不覆盖丢弃（防花屏）
   │  pop                                                   Decoder 线程
   ▼
avcodec_send_packet / avcodec_receive_frame（优先 <编码名>_cuvid；设备创建或打开失败回退软解）
   │  硬件帧先 av_hwframe_transfer_data 下载到主机内存；已是 NV12 则直接产出（加引用/接管，零拷贝）
   │  只有真正的格式变更（YUV420P/P010/YUV444P…）才走 sws_getCachedContext → AV_PIX_FMT_NV12
   │  （偶数宽高，自动适配中途换分辨率）
   ▼
avframe::VideoFramePtr { AVFrameHolder, ptsSeconds, recvAt, decodedAt }
   │  解码线程回调 → 只入队（FrameQueue，容量 3，满则丢最旧，绝不阻塞解码线程）
   ▼
FrameQueue::popForDisplay（GUI 线程，控件空闲时取一帧）→ 适配成 Nv12FrameView + FrameLease
   ▲                                  │
   └── vsync 节拍：VideoWidget::frameSwapped（一帧真正上屏后立刻喂下一帧）
   ▼
VideoWidget::onFrameReady → paintGL：上传 NV12 双纹理 → YUV→RGB 着色器（宽高比 letterbox）
```

* 停止顺序由 Pipeline 唯一决定：`queue.close()`（先解阻塞，否则 demuxer 阻塞在满队列 push 上会 join 死锁）→ `demuxer.stop()` → `decoder.stop()`。
* `Pipeline::isRunning()` 只在**解码线程彻底收工**后转 false：上游上报"流已结束"时解码线程可能还在回调残留帧，过早收尾会截断画面。
* **上屏节拍由显示器的 vsync 决定，而不是固定周期定时器**：`frameSwapped()` 表示"本帧已完成上屏"，
  应用层此刻立刻取下一帧（`framePending()` 为 false 才允许喂），因此不会像 16ms 定时器那样与
  60Hz 的 16.67ms 拍频、也不会在控件还没画完时连喂两帧。4ms 的 `QTimer` 只做**补漏**
  （队列取空后链条会停，下一帧到达时重新启动），不参与呈现节拍，只在连接期间运行。
* **帧投递用"有界队列 + vsync 拉取"而非跨线程信号**：解码线程只 `push`（自带锁、不阻塞），
  不产生事件洪水；队列把上游抖动与本地呈现解耦，丢弃决策基于时间跨度而不是"槽位是否被占"。
* 两类丢弃分开计数（状态栏"丢 满 x / 过期 y"）：`满` = 入队时队列已满丢最旧（上游产出快于上屏消费）；
  `过期` = 出队时积压跨度超过 60ms，跳到最新帧追赶（本地呈现落后）。
  实测：带音轨的 1080p60 RTSP 流会把视频帧成簇投递（两帧贴脸 <1ms + 约 21ms 空档），
  旧的"最新帧单槽 + 16ms 定时器"在这种流上只能上屏 47fps 且每秒丢 12~15 帧；
  换成容量 3 的队列 + vsync 节拍后为 60.0fps、零丢弃（同一台机器、同一条流复测）。
* 像素规整只在**真正的格式变更**时才允许走 swscale：libswscale 对"同格式 + 同尺寸"（例如 NV12→NV12）
  走的是只拷亮度平面、**完全不写色度平面**的快速通道（`planarCopyWrapper` 因 NV12 的 `data[2]` 为空而跳过 UV），
  输出帧色度保持未初始化的 0，渲染出来就是"整幅发绿、画面仍在动"。硬件帧下载（`av_hwframe_transfer_data`）
  的产物恰好就是 NV12，所以这条路径正是硬件解码画面变绿的根因；解码输出已经是 NV12 时直接产出（零拷贝）。

## 2. 模块边界与职责

| 模块 | 唯一职责 | 明确不做 | 可依赖 | 线程 | 拥有 |
|---|---|---|---|---|---|
| `Decoder/RingBuffer.hpp` | 通用带锁环形队列（容量/满策略/唤醒/关闭） | 不认识包与帧，无业务语义 | STL | 无 | 缓冲槽位 |
| `Decoder/PacketData.h` | FFmpeg RAII 与跨线程 DTO | 无逻辑、无策略 | FFmpeg | 无 | RAII 所有权 |
| `Decoder/Demuxer.{h,cpp}` | URL → 编码包序列（探测、超时、中断、只推视频包） | 不解码、不知 NV12、不 close 队列 | RingBuffer/PacketData/FFmpeg | 自有 1 线程 | `AVFormatContext` |
| `Decoder/Decoder.{h,cpp}` | 编码包 → NV12 帧（解码上下文、硬件优先/软解回退、格式与尺寸归一化、pts） | 不拉流、不管网络、不拥有队列；不做像素以外的策略 | RingBuffer/PacketData/FFmpeg | 自有 1 线程 | `AVCodecContext`（含其持有的硬件设备引用）/`SwsContext` |
| `Decoder/Pipeline.{h,cpp}` | 装配与生命周期编排（队列归属、启停顺序、回调转发），pimpl 隐藏内部 | 不实现拉流/解码细节 | Demuxer/Decoder | 编排者 | 包队列（唯一 `close/reset` 处） |
| `VideoWidget/*` | NV12 视图 → GPU 上屏（GL 资源、着色器、宽高比、上屏帧率） | 不知帧从哪来、**不依赖 FFmpeg/Pipeline**；不做队列/丢帧策略 | Qt + `nv12frameview.h` | 仅 GUI 线程 | GL 纹理/VAO/VBO、当前帧保活句柄 |
| `framequeue.{h,cpp}` | 应用层呈现队列：有界 FIFO + 两类丢弃计数（容量/积压跨度上限） | 不认识 Qt、不认识控件、不拉流不解码、不阻塞不等待 | STL + `Decoder/PacketData.h`（公共 DTO） | 无（push 在解码线程、pop 在 GUI 线程，内部加锁） | 队列内待显示帧的所有权 |
| `mainwindow.{h,cpp}` | 交互 + 跨线程投递 + 上屏调度 + 状态展示 + 视图适配器 | 不改帧内容、不做拉流/解码、不做 GL | Pipeline/VideoWidget/FrameQueue | 仅 GUI 线程（回调只入队） | `Pipeline`、`FrameQueue`、UI 对象 |

**边界靠构建系统强制**：`VideoWidget` 是独立目标，不链接 `Decoder`、也不带它的 include 目录，因此控件物理上无法包含 FFmpeg/Pipeline 头；
`ringbuffer_test` 只链接 `DecoderPrimitives`（纯 header 接口目标），不链接 FFmpeg。
公共头只有 `Decoder/Pipeline.h` 与 `Decoder/PacketData.h`，其余标注为内部头文件。

## 3. 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-20
cmake --build build -j
```
依赖：Qt 6.5+（Core/Widgets/OpenGL/OpenGLWidgets）、FFmpeg 7（libavcodec/libavformat/libavutil/libswscale）、CMake 3.19+、支持 C++20 的编译器。
硬件解码需要 NVIDIA 驱动 + FFmpeg 编入对应 `*_cuvid` 解码器；**缺失或设备不可用时自动回退软解**（实际生效的解码器见启动日志里的 `codec=`，如 `h264_cuvid` / `h264`）。
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
* `framequeue`：应用层呈现队列的语义回归——空队列出队、严格 FIFO、满队列丢最旧（`overflowDropped`）、
  积压跨度超过 60ms 才跳到最新帧（`staleSkipped`）、跨度等于上限不跳帧、`clear()` 不计丢弃、`resetStats()` 归零，
  以及单生产者/单消费者 2 万帧的计数不变量（`到达 = 上屏 + 满丢弃 + 过期跳过`）与出队序列严格递增。
* `pipeline_smoke_file`：自动生成素材（`rawvideo + /dev/zero`，本机 FFmpeg 未编 avdevice 故不用 lavfi）后解码校验。
* `pipeline_smoke_nv12`：**绿屏回归用例**。素材是容器里的 `rawvideo(NV12)` 流（随机 RGB 转 NV12，故色度必然非常量），
  解码输出**就是 NV12**，因此必然走"源格式已是目标格式"这条分支；用 `--require-chroma` 断言色度平面非常量
  （色度整片为常量 = 色度未被写入 = 画面整幅发绿）。无需 GPU，可直接复现修复前的行为。
* 手动：`./build/tests/pipeline_smoke <地址> [--frames N] [--require-chroma]` 校验 NV12/偶数尺寸/色度内容/停机 <500ms
  （素材本身色度平坦时不要加 `--require-chroma`，例如全零画面）。

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
GUI 上屏实测（RTX 3050 Laptop，h264_cuvid 硬解，窗口 1308x786，状态栏读数）：

| 流 | 修复前（单槽 + 16ms 定时器） | 修复后（3 帧队列 + vsync 节拍） |
|---|---|---|
| 1920x1080@59.94 + AAC | 上屏 46.9~50.6 fps，丢 12~15 帧/秒 | **上屏 60.0 fps，丢 满 0~1 / 过期 0**（12s 采样） |
| 3840x2160@60 + AAC | 上屏 50.2 fps，丢 ~10 帧/秒 | **上屏 60.0 fps，丢 满 36（仅启动瞬态）/ 过期 0** |
| 1920x1080@120（超出上屏能力） | — | 上屏 60.0 fps，丢 满 ~62 帧/秒、过期 0（超出部分正常丢弃） |
| 3840x2160@60 仅视频 | 上屏 57.3 fps | 上屏 60.0 fps，丢 满 36 / 过期 0 |

（`丢 满` = 上游产出快于上屏消费；`丢 过期` = 本地呈现落后、按积压跨度主动跳帧。端到端延迟 12~27ms。）
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
* `tests/sanitizer/lsan.supp` 过滤第三方（FFmpeg/Qt/X11）一次性分配；`tests/sanitizer/tsan.supp` 过滤未插桩的 Qt/FFmpeg/dbus/glib/fontconfig 噪声——本项目源码零报告（GUI 运行时如出现涉及 `Decoder/`、`VideoWidget/`、`mainwindow.cpp`、`framequeue.cpp` 的报告，请先删掉对应抑制行再复现）。
* **TSan 必须在禁用 CUDA 的前提下跑**：`CUDA_VISIBLE_DEVICES= ctest --test-dir build-tsan`。
  未插桩的 NVIDIA 解码栈（libcuda/libnvcuvid + FFmpeg 的 cuvid 封装）在 TSan 下会以
  `SEGV on unknown address 0x000000000c38` 崩溃，**与本项目代码无关**（`pipeline_smoke` 这种完全不涉及上屏路径的用例同样复现）；
  禁用 CUDA 后解码器按设计回退软解，6/6 用例通过、GUI 实跑零报告。ASan 侧因影子内存会让 `cuInit` 失败，天然走软解。
* GUI 的 sanitizer 实跑建议用软件 GL（`Xvfb :77 -screen 0 1280x800x24` + `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe`）：
  显卡驱动未插桩，混在一起无法判定报告归属。可复用 `.cache/analysis/sanitizer_gui_probe.sh`（该脚本会自建显示、自推流并留存报告）。

## 7. 已知限制（本轮非目标）

音频解码/播放与音视频同步、**按 PTS 到点呈现**（当前是"vsync 节拍 + 有界队列 + 积压跨度跳帧"：不建本地参考时钟，因此不处理源 PTS 跳变/暂停同步；README 的显示延迟以"收包→上屏"衡量）、包池/帧池、自动重连、截图/全屏/录制、GL 不可用时的光栅降级渲染、GPU 内直接上屏（当前硬件帧下载后仍是内存 NV12）、无锁队列（`Decoder/readerwriterqueue-1.0.7/` 保留未接入）。
呈现队列容量 3 帧是"抗抖动"与"延迟/内存"的折中：1080p 保活约 9MB、4K 约 37MB，端到端延迟比单槽多 1~2 帧；
队列只在**积压跨度 > 60ms**（本地确实落后）时跳帧，正常播放不跳帧、不丢帧。
颜色范围未贯通：`YUVJ*`（全范围）源在进入 swscale 前被规整成对应 `YUV*`，着色器也固定按 BT.601 limited range 解释，全范围素材会偏灰；要修需把 `color_range`/`colorspace` 经 `sws_setColorspaceDetails` 与 uniform 一路传下去。
`mpeg1video`/`mpeg2video` 的 cuvid 解码器名与描述符名不一致（实际是 `mpeg1_cuvid`/`mpeg2_cuvid`），当前会静默回退软解。
无 GPU 的机器上 FFmpeg 会向 stderr 打印一行 CUDA 初始化失败日志（未安装 ffmpeg 日志回调，仅噪声、不影响回退）。
显示需要可用的 OpenGL 3.3 Core 环境：上下文创建失败时程序会在日志中给出可执行诊断（`QT_XCB_GL_INTEGRATION=xcb_egl`、`LIBGL_ALWAYS_SOFTWARE=1`），但不会回退到软件绘制。
