#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "PacketData.h"
#include "PacketQueue.h"

/// @file Demuxer.h
/// @brief 拉流/解复用：把 URL（rtsp:// | rtp://(SDP) | 本地文件）变成编码包序列。
///
/// 职责边界：
///  - 只做"打开输入 → 读包 → 推送视频包"；
///  - 不解码、不关心像素格式、不拥有队列（队列由 Pipeline 拥有并负责 close/reset），
///    也不得触碰任何 Qt 对象。
///
/// 线程：自有 1 个 jthread；open() 在调用线程执行（受超时/中断保护），
///       workLoop() 在工作线程执行，结束后通过 StatusCallback 上报"流结束/拉流失败"。
///
/// 内部头文件：仅供 Decoder 库内部使用。

class Demuxer {
public:
    /// 探测结果：视频流必需，音频流可选（本轮不消费，仅保留供后续使用）。
    struct StreamInfo {
        AVStream* video = nullptr;
        AVStream* audio = nullptr;
        std::string codecName;
        std::string formatName;
        int width = 0;
        int height = 0;
    };

    Demuxer() = default;
    ~Demuxer();
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;
    Demuxer(Demuxer&&) = delete;
    Demuxer& operator=(Demuxer&&) = delete;

    /// 状态出口（流结束 / 拉流失败），在拉流线程上调用；空消息不会上报。
    void setStatusCallback(videopipe::StatusCallback callback) { m_onStatus = std::move(callback); }

    /// 打开输入并探测流信息（阻塞，受 m_readTimeout 与 RTSP 超时选项保护）。
    /// @param url   输入地址
    /// @param queue 由 Pipeline 拥有、此处仅借用的包队列
    /// @param error 失败时回填可读原因
    [[nodiscard]] std::optional<StreamInfo> open(std::string_view url, PacketQueue* queue, std::string* error);

    /// 释放输入资源。调用前必须先 stop()（~Demuxer 已保证顺序）。
    void close();

    void start();  // 启动拉流线程
    void stop();   // 请求停止并 join（中断回调保证能立刻打断阻塞的 IO）

private:
    static int interruptCallback(void* opaque);
    [[nodiscard]] bool interrupted() const noexcept;
    void workLoop(std::stop_token stopToken);
    [[nodiscard]] static std::int64_t steadyNowMs() noexcept;

    std::string m_url;
    AVFormatContext* m_format = nullptr;
    PacketQueue* m_queue = nullptr;   // 非拥有：队列生命周期归 Pipeline

    int m_videoStreamIndex = -1;

    std::atomic_bool m_abort{false};
    std::atomic<std::int64_t> m_lastProgressMs{0};  // 最近一次成功读包的时刻
    std::chrono::milliseconds m_readTimeout{15000}; // 连续无数据超过该时长即判定卡死

    videopipe::StatusCallback m_onStatus;
    std::jthread m_worker;
};
