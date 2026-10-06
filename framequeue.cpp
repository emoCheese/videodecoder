#include "framequeue.h"

#include <utility>

/// @file framequeue.cpp
/// @brief FrameQueue 的实现：只做有界队列与两类丢弃计数，不含任何线程/时序策略。

FrameQueue::FrameQueue(std::size_t capacity, std::chrono::milliseconds maxSpan)
    : m_capacity(capacity > 0 ? capacity : 1),  // 容量 0 会让 push 无法容纳任何帧，按 1 兜底
      m_maxSpan(maxSpan)
{
}

void FrameQueue::push(avframe::VideoFramePtr frame, std::chrono::steady_clock::time_point arrivedAt)
{
    if (!frame) {
        return;  // 空帧不入队，也不计入到达数
    }

    std::lock_guard lock(m_mutex);
    ++m_stats.arrived;

    if (m_entries.size() >= m_capacity) {
        // 满：丢最旧。视频帧可以丢（丢的是画面流畅度），不像视频包会破坏参考帧链导致花屏。
        m_entries.pop_front();
        ++m_stats.overflowDropped;
    }
    m_entries.push_back(Entry{std::move(frame), arrivedAt});
}

avframe::VideoFramePtr FrameQueue::popForDisplay()
{
    std::lock_guard lock(m_mutex);
    if (m_entries.empty()) {
        return nullptr;
    }

    // 积压跨度 = 队尾(最新) − 队首(最旧)。跨度超过阈值说明本地呈现已经落后于上游，
    // 继续按 FIFO 播放只会让延迟越积越大，因此跳到最新帧追赶。
    // 注意：不能用"队列深度"单独作为判据 —— 一次正常的突发（例如两帧贴脸 + 一帧）
    // 也会把队列填满，那属于"可以按序播完"的情况，跳帧反而会重新引入丢帧。
    const auto span = m_entries.back().arrivedAt - m_entries.front().arrivedAt;
    if (m_entries.size() > 1 && span > m_maxSpan) {
        m_stats.staleSkipped += m_entries.size() - 1;
        avframe::VideoFramePtr newest = std::move(m_entries.back().frame);
        m_entries.clear();
        return newest;
    }

    avframe::VideoFramePtr frame = std::move(m_entries.front().frame);
    m_entries.pop_front();
    return frame;
}

void FrameQueue::clear()
{
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

FrameQueue::Stats FrameQueue::stats() const
{
    std::lock_guard lock(m_mutex);
    return m_stats;
}

void FrameQueue::resetStats()
{
    std::lock_guard lock(m_mutex);
    m_stats = Stats{};
}

std::size_t FrameQueue::size() const
{
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}
