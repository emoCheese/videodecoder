#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

/// @file RingBuffer.hpp
/// @brief 通用多生产者/多消费者环形队列（带锁实现，简单可靠优先）。
///
/// 职责边界：
///  - 只提供容量、满策略、阻塞/唤醒与关闭语义；
///  - 不认识 AVPacket / VideoFrame，不含任何业务语义；
///  - 内部保证线程安全，但不拥有、不创建线程。
///
/// 生命周期约定（见项目模块边界约定）：
///  - close()/reset() 的调用权归队列拥有者（本项目中为 Pipeline）；
///    push()/pop() 的使用者（Demuxer/Decoder）只借用队列，不得关闭或复位它。
///
/// 内部头文件：仅供 Decoder 库内部与 DecoderPrimitives 单元测试使用。

/// 队列满时的策略：阻塞等待空位。
struct BlockingFullPolicy {
    static constexpr bool block_on_full = true;
};

/// 队列满时的策略：覆盖最旧元素（丢帧语义，仅适用于可丢弃的数据）。
struct OverwriteFullPolicy {
    static constexpr bool block_on_full = false;
};

template <typename T, std::size_t Capacity, typename FullPolicy = BlockingFullPolicy>
class RingBuffer {
    static_assert(Capacity > 0, "RingBuffer 容量必须大于 0");

public:
    RingBuffer() = default;
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    /// 入队（阻塞策略下满时等待；覆盖策略下满时丢弃最旧元素）。
    /// @return 队列已关闭时返回 false，元素被丢弃，调用方应据此退出生产循环。
    [[nodiscard]] bool push(T item)
    {
        std::unique_lock lock(m_mutex);

        if constexpr (FullPolicy::block_on_full) {
            m_notFull.wait(lock, [this] { return m_closed || m_size < Capacity; });
            if (m_closed) {
                return false;
            }
        }
        else {
            if (m_closed) {
                return false;
            }
            if (m_size == Capacity) {  // 覆盖最旧：head_/tail_ 同步前进，容量占用保持不变
                m_buffer[m_head].emplace(std::move(item));
                m_head = (m_head + 1) % Capacity;
                m_tail = (m_tail + 1) % Capacity;
                m_notEmpty.notify_one();
                return true;
            }
        }

        m_buffer[m_tail].emplace(std::move(item));
        m_tail = (m_tail + 1) % Capacity;
        ++m_size;
        m_notEmpty.notify_one();
        return true;
    }

    /// 出队（队空时阻塞）。
    /// @return 队列已关闭且已取空时返回 false，调用方应据此退出消费循环。
    [[nodiscard]] bool pop(T& out)
    {
        std::unique_lock lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return m_closed || m_size > 0; });

        if (m_size == 0) {  // 只能是 closed && size == 0
            return false;
        }

        out = std::move(m_buffer[m_head].value());
        m_buffer[m_head].reset();
        m_head = (m_head + 1) % Capacity;
        --m_size;
        if constexpr (FullPolicy::block_on_full) {
            m_notFull.notify_one();
        }
        return true;
    }

    /// 关闭队列：唤醒全部等待者；此后 push 直接失败，pop 取空后失败。幂等。
    void close()
    {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    /// 重新打开并清空（供再次开始拉流时复用同一队列）。
    void reset()
    {
        {
            std::lock_guard lock(m_mutex);
            for (auto& slot : m_buffer) {
                slot.reset();
            }
            m_head = 0;
            m_tail = 0;
            m_size = 0;
            m_closed = false;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    [[nodiscard]] bool closed() const
    {
        std::lock_guard lock(m_mutex);
        return m_closed;
    }

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard lock(m_mutex);
        return m_size;
    }

    [[nodiscard]] bool empty() const
    {
        std::lock_guard lock(m_mutex);
        return m_size == 0;
    }

    [[nodiscard]] bool full() const
    {
        std::lock_guard lock(m_mutex);
        return m_size == Capacity;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    std::array<std::optional<T>, Capacity> m_buffer{};
    std::size_t m_head = 0;
    std::size_t m_tail = 0;
    std::size_t m_size = 0;
    bool m_closed = false;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
};
