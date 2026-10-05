#pragma once

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

/// @file PacketData.h
/// @brief FFmpeg RAII 包装与跨线程传输对象（DTO）。
///
/// 职责边界：
///  - 只负责所有权与数据承载，不含任何拉流/解码/队列策略逻辑；
///  - 公共头文件：应用层（含 Qt 界面层）可以包含它来读取帧数据。
///
/// @todo 后续可增加 AVPacket / AVFrame 池，避免每帧 av_frame_alloc。

namespace videopipe {

/// 库向应用层上报状态/错误的统一出口。
/// 注意：在库的工作线程上被调用，实现内不得阻塞、不得直接触碰 GUI 对象。
using StatusCallback = std::function<void(std::string message)>;

}  // namespace videopipe

namespace avpacket {

struct PacketDeleter {
    void operator()(AVPacket* pkt) const noexcept
    {
        if (pkt != nullptr) {
            av_packet_free(&pkt);
        }
    }
};

using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

/// 分配一个空包，失败返回 nullptr。
[[nodiscard]] inline PacketPtr create()
{
    return PacketPtr(av_packet_alloc());
}

/// 队列元素：编码包 + 该包进入管线的时刻（用于端到端延迟统计）。
struct Packet {
    PacketPtr pkt;
    std::chrono::steady_clock::time_point recvAt{};
};

}  // namespace avpacket

namespace avframe {

/// AVFrame 独占所有权的 RAII 持有者（仅可移动）。
class AVFrameHolder {
public:
    AVFrameHolder() = default;
    explicit AVFrameHolder(AVFrame* frame) noexcept : m_frame(frame) {}
    ~AVFrameHolder() { reset(); }

    AVFrameHolder(const AVFrameHolder&) = delete;
    AVFrameHolder& operator=(const AVFrameHolder&) = delete;

    AVFrameHolder(AVFrameHolder&& other) noexcept
        : m_frame(std::exchange(other.m_frame, nullptr))
    {
    }

    AVFrameHolder& operator=(AVFrameHolder&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_frame = std::exchange(other.m_frame, nullptr);
        }
        return *this;
    }

    /// 释放内部帧（幂等；只释放，不接受新指针，避免自赋值悬空）。
    void reset() noexcept
    {
        if (m_frame != nullptr) {
            av_frame_free(&m_frame);
        }
    }

    /// 交出所有权（调用后本对象为空）。
    [[nodiscard]] AVFrame* release() noexcept { return std::exchange(m_frame, nullptr); }

    [[nodiscard]] AVFrame* get() const noexcept { return m_frame; }
    [[nodiscard]] AVFrame* operator->() const noexcept { return m_frame; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_frame != nullptr; }

private:
    AVFrame* m_frame = nullptr;
};

/// 分配一个新的空 AVFrame。
[[nodiscard]] inline AVFrameHolder create()
{
    return AVFrameHolder(av_frame_alloc());
}

/// 一帧已解码并转换为 NV12 的视频数据（库 → 应用层的传递单元）。
struct VideoFrame {
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    AVFrameHolder frame;   ///< NV12 帧：data[0]=Y，data[1]=UV（交错）
    double ptsSeconds = 0.0;
    std::chrono::steady_clock::time_point recvAt{};     ///< 所属编码包进入管线的时刻
    std::chrono::steady_clock::time_point decodedAt{};  ///< 解码完成时刻
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;

}  // namespace avframe
