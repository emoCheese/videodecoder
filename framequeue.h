#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>

#include "PacketData.h"  // 库的公共 DTO：avframe::VideoFramePtr

/// @file framequeue.h
/// @brief 应用层的"呈现队列"：解码线程入队，GUI 线程按上屏节拍出队。
///
/// 职责边界（对应 README 模块边界表）：
///  - 只做"有界帧队列 + 丢弃计数"这一件事：不认识 Qt、不认识渲染控件、不拉流不解码；
///  - 只使用 Decoder 库的公共 DTO（avframe::VideoFramePtr），不触碰 Pipeline 的内部类型；
///  - 不阻塞、不等待、不创建线程：入队满时丢最旧（视频帧可丢，与"视频包不可覆盖丢弃"相反），
///    出队遇空队列立即返回 nullptr；
///  - 线程安全：push() 在解码线程，popForDisplay()/clear() 在 GUI 线程。
///
/// 为什么需要它（替代原先的"最新帧单槽"）：
///  单槽只能保留 1 帧，而实测带音轨的 RTSP 流会把视频帧**成簇**投递
///  （两帧贴脸 <1ms + 约 21ms 空档）：同一个 GUI 定时器周期内到达的第 2 帧必然被覆盖丢弃，
///  1080p60 的流因此只能上屏 47 fps。容量 3 的小队列把"丢弃决策"从"槽位是否被占"
///  改成"帧的时间跨度/水位"，成簇的帧对得以摊到相邻两次上屏，不再丢帧。
///
/// 丢弃语义（两类分开计数，供状态栏区分"上游过快"与"本地落后"）：
///  - overflowDropped：入队时队列已满，丢掉最旧一帧 —— 上游产出快于上屏消费；
///  - staleSkipped   ：出队时判定积压跨度过大，跳过旧帧直接取最新 —— 本地呈现落后，主动追赶。

class FrameQueue {
public:
    /// 容量 3：足以吸收实测的"两帧贴脸"簇（1080p 保活约 9MB，4K 约 37MB）。
    static constexpr std::size_t kDefaultCapacity = 3;
    /// 积压跨度上限 60ms：队首与队尾的到达时刻相差超过它就跳到最新帧追赶。
    static constexpr std::chrono::milliseconds kDefaultMaxSpan{60};

    /// 队列计数（内部持锁更新，读取取快照）。
    struct Stats {
        unsigned long long arrived = 0;          ///< 进入队列的帧数（用于算"到达 fps"）
        unsigned long long overflowDropped = 0;  ///< 队列满导致的丢弃
        unsigned long long staleSkipped = 0;     ///< 过期跳过（本地主动追赶）
    };

    explicit FrameQueue(std::size_t capacity = kDefaultCapacity,
                        std::chrono::milliseconds maxSpan = kDefaultMaxSpan);
    ~FrameQueue() = default;

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;
    FrameQueue(FrameQueue&&) = delete;
    FrameQueue& operator=(FrameQueue&&) = delete;

    /// 入队（解码线程调用，永不阻塞）。空帧被忽略且不计数。
    /// @param frame     解码产出的 NV12 帧（队列接管其所有权，直到被取走或丢弃）
    /// @param arrivedAt 到达时刻；默认取当前时刻，单元测试可显式注入以获得确定性
    void push(avframe::VideoFramePtr frame,
              std::chrono::steady_clock::time_point arrivedAt = std::chrono::steady_clock::now());

    /// 出队取一帧上屏（GUI 线程调用，永不阻塞）。
    /// 默认严格 FIFO（保证成簇到达的帧一帧不丢）；仅当积压跨度超过 maxSpan 时
    /// 跳到最新帧，并把被跳过的帧计入 staleSkipped。
    /// @return 队列为空时返回 nullptr。
    [[nodiscard]] avframe::VideoFramePtr popForDisplay();

    /// 丢弃队列内所有帧（断开/重连时调用）。这是生命周期动作，不计入丢弃统计。
    void clear();

    [[nodiscard]] Stats stats() const;
    /// 计数归零（队列内容保留）：连接新地址时从零开始统计。
    void resetStats();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] std::chrono::milliseconds maxSpan() const noexcept { return m_maxSpan; }

private:
    struct Entry {
        avframe::VideoFramePtr frame;
        std::chrono::steady_clock::time_point arrivedAt{};
    };

    const std::size_t m_capacity;
    const std::chrono::milliseconds m_maxSpan;

    mutable std::mutex m_mutex;
    std::deque<Entry> m_entries;  // 队首 = 最旧，队尾 = 最新
    Stats m_stats{};
};
