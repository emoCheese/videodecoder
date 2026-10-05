/// @file pipeline_smoke.cpp
/// @brief 管线烟测：驱动 Pipeline 解码，校验帧数据与停机时限。
///
/// 用法: pipeline_smoke <url|path> [--frames N] [--timeout-ms M]
///   <url|path>      rtsp://... | rtp 的 SDP 文件路径 | 本地视频文件
///   --frames N      期望解码帧数，达到即结束（默认 60；0 表示跑到超时或流结束）
///   --timeout-ms M  总超时（默认 30000）
///
/// 校验：帧非空、NV12、宽高为正且为偶数、行宽足够；stop() 必须在 500 ms 内返回。
/// 退出码：0 通过；1 校验失败；2 启动失败（例如地址不可达）。

#include "Pipeline.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr int kStopDeadlineMs = 500;

struct Stats {
    std::mutex mutex;
    long frames = 0;
    long badFrames = 0;
    int width = 0;
    int height = 0;
    std::string status;
    bool finished = false;
    std::chrono::steady_clock::time_point firstFrameAt{};
    std::chrono::steady_clock::time_point lastFrameAt{};
};

void printUsage(const char* program)
{
    std::printf("用法: %s <rtsp://... | rtp.sdp | 本地文件> [--frames N] [--timeout-ms M]\n", program);
}

bool takeFrame(avframe::VideoFramePtr frame, Stats& stats)
{
    const AVFrame* raw = frame ? frame->frame.get() : nullptr;
    if (raw == nullptr || frame->format != AV_PIX_FMT_NV12 || frame->width <= 0 || frame->height <= 0 ||
        (frame->width % 2) != 0 || (frame->height % 2) != 0 || raw->data[0] == nullptr ||
        raw->data[1] == nullptr || raw->linesize[0] < frame->width || raw->linesize[1] < frame->width / 2) {
        ++stats.badFrames;
        return false;
    }
    stats.width = frame->width;
    stats.height = frame->height;
    stats.lastFrameAt = std::chrono::steady_clock::now();
    if (stats.frames == 1) {
        stats.firstFrameAt = stats.lastFrameAt;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printUsage(argv[0]);
        return 2;
    }

    const std::string url = argv[1];
    long expectedFrames = 60;
    int timeoutMs = 30000;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            expectedFrames = std::strtol(argv[++i], nullptr, 10);
        }
        else if (std::strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            timeoutMs = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        }
        else {
            std::printf("未知参数: %s\n", argv[i]);
            printUsage(argv[0]);
            return 2;
        }
    }

    Stats stats;
    Pipeline pipeline;
    pipeline.setFrameCallback([&stats](avframe::VideoFramePtr frame) {
        std::lock_guard lock(stats.mutex);
        ++stats.frames;
        takeFrame(std::move(frame), stats);
    });
    pipeline.setStatusCallback([&stats](std::string message) {
        std::lock_guard lock(stats.mutex);
        stats.status = std::move(message);
        stats.finished = true;
    });

    std::printf("[smoke] 输入: %s\n", url.c_str());
    const auto startAt = std::chrono::steady_clock::now();

    std::string error;
    if (!pipeline.start(url, &error)) {
        std::printf("[smoke] 启动失败: %s\n", error.c_str());
        return 2;
    }
    std::printf("[smoke] 已启动: codec=%s size=%dx%d\n",
                pipeline.codecName().c_str(),
                pipeline.width(),
                pipeline.height());

    const auto deadline = startAt + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard lock(stats.mutex);
            if (expectedFrames > 0 && stats.frames >= expectedFrames) {
                break;
            }
            // 上游上报结束后不能立刻停：解码线程还要把残留帧回调完，
            // isRunning() 转 false 才代表管线彻底收工。
            if (stats.finished && !pipeline.isRunning()) {
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 停机时限是"简单可靠"的硬指标：必须先解阻塞再 join，不能卡住。
    const auto stopBegin = std::chrono::steady_clock::now();
    pipeline.stop();
    const auto stopMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopBegin).count();

    long frames = 0;
    long badFrames = 0;
    int width = 0;
    int height = 0;
    std::string status;
    std::chrono::steady_clock::time_point firstFrameAt{};
    std::chrono::steady_clock::time_point lastFrameAt{};
    {
        std::lock_guard lock(stats.mutex);
        frames = stats.frames;
        badFrames = stats.badFrames;
        width = stats.width;
        height = stats.height;
        status = stats.status;
        firstFrameAt = stats.firstFrameAt;
        lastFrameAt = stats.lastFrameAt;
    }

    double fps = 0.0;
    const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(lastFrameAt - firstFrameAt).count();
    if (frames > 1 && span > 0) {
        fps = static_cast<double>(frames - 1) * 1000.0 / static_cast<double>(span);
    }

    std::printf("[smoke] 帧数=%ld 坏帧=%ld 分辨率=%dx%d 解码FPS=%.1f 停机=%lldms\n",
                frames,
                badFrames,
                width,
                height,
                fps,
                static_cast<long long>(stopMs));
    if (!status.empty()) {
        std::printf("[smoke] 状态上报: %s\n", status.c_str());
    }

    bool ok = true;
    if (badFrames != 0) {
        std::printf("[smoke] 失败: 存在不符合 NV12/偶数尺寸约定的帧\n");
        ok = false;
    }
    if (expectedFrames > 0 && frames < expectedFrames) {
        std::printf("[smoke] 失败: 期望至少 %ld 帧，实际 %ld 帧\n", expectedFrames, frames);
        ok = false;
    }
    if (frames == 0) {
        std::printf("[smoke] 失败: 一帧都没有解出来\n");
        ok = false;
    }
    if (stopMs > kStopDeadlineMs) {
        std::printf("[smoke] 失败: stop() 耗时 %lldms 超过 %dms 上限\n",
                    static_cast<long long>(stopMs),
                    kStopDeadlineMs);
        ok = false;
    }

    std::printf("[smoke] %s\n", ok ? "通过" : "未通过");
    return ok ? 0 : 1;
}
