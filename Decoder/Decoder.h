#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "PacketData.h"
#include "PacketQueue.h"

/// @file Decoder.h
/// @brief 视频解码：编码包 → NV12 帧。
///
/// 职责边界：
///  - 只做"送包 → 取帧 → 转 NV12 → 回调"；
///  - 不拉流、不管网络、不拥有队列（队列由 Pipeline 拥有并负责 close/reset），
///    不触碰任何 Qt 对象。
///
/// 线程：自有 1 个 jthread 消费包队列；帧通过 FrameCallback 在工作线程上回调给应用层。
///
/// 内部头文件：仅供 Decoder 库内部使用。

/// 帧消费者约束：可用 avframe::VideoFramePtr 调用。
template <typename F>
concept FrameSink = std::invocable<F, avframe::VideoFramePtr>;

class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    /// 帧出口：在工作线程上被调用，实现内不得阻塞、不得直接触碰 GUI 对象。
    template <FrameSink F>
    void setFrameCallback(F&& sink)
    {
        m_onFrame = std::forward<F>(sink);
    }

    /// 状态出口（解码失败），在工作线程上被调用。
    void setStatusCallback(videopipe::StatusCallback callback) { m_onStatus = std::move(callback); }

    /// 结束出口：解码线程即将退出（残留帧已全部回调完毕）时调用一次，仍在工作线程上。
    void setFinishedCallback(std::function<void()> callback) { m_onFinished = std::move(callback); }

    /// 初始化解码器。
    /// @param stream 视频流（来自 Demuxer::StreamInfo）
    /// @param queue  由 Pipeline 拥有、此处仅借用的包队列
    /// @param error  失败时回填可读原因
    [[nodiscard]] bool open(const AVStream* stream, PacketQueue* queue, std::string* error);

    /// 释放解码资源。调用前必须先 stop()（~Decoder 已保证顺序）。
    void close();

    void start();  // 启动解码线程
    /// 请求停止并 join。
    /// @warning 调用前必须已关闭包队列（Pipeline::stop 负责），否则阻塞在 pop 上的线程无法唤醒。
    void stop();

    [[nodiscard]] int width() const noexcept { return m_width.load(std::memory_order_relaxed); }
    [[nodiscard]] int height() const noexcept { return m_height.load(std::memory_order_relaxed); }
    [[nodiscard]] std::string codecName() const { return m_codecName; }

private:
    bool sendPacket(const AVPacket* packet, std::string* error);
    bool receiveFrames(std::string* error);
    bool convertAndEmit(const AVFrame* source, std::string* error);
    bool flush(std::string* error);
    void workerLoop(std::stop_token stopToken);
    [[nodiscard]] double framePtsSeconds(const AVFrame* frame) const noexcept;

    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    AVRational m_timeBase{0, 1};
    PacketQueue* m_queue = nullptr;  // 非拥有：队列生命周期归 Pipeline

    std::string m_codecName;
    std::atomic<int> m_width{0};
    std::atomic<int> m_height{0};

    /// 当前正在处理的包进入管线的时刻；用于给本批解码帧打延迟基准。
    std::chrono::steady_clock::time_point m_lastRecvAt{};

    std::function<void(avframe::VideoFramePtr)> m_onFrame;
    videopipe::StatusCallback m_onStatus;
    std::function<void()> m_onFinished;
    std::jthread m_worker;
};
