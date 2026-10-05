#include "Pipeline.h"

#include <atomic>
#include <memory>
#include <utility>

#include "Decoder.h"
#include "Demuxer.h"
#include "PacketQueue.h"

/// @file Pipeline.cpp
/// @brief 装配与生命周期编排：队列归属、启停顺序、回调转发。

/// 管线内部实现：应用层不可见。
struct Pipeline::Impl {
    /// 视频包队列：创建、close()、reset() 只发生在 Pipeline 代码内。
    PacketQueue packets;
    std::unique_ptr<Demuxer> demuxer;
    std::unique_ptr<Decoder> decoder;
    std::atomic_bool running{false};
    Pipeline::FrameCallback onFrame;
    Pipeline::StatusCallback onStatus;
};

Pipeline::Pipeline() : m_impl(std::make_unique<Impl>()) {}

Pipeline::~Pipeline()
{
    stop();
}

bool Pipeline::start(std::string_view url, std::string* error)
{
    Impl& impl = *m_impl;
    stop();                // 幂等：支持反复连接
    impl.packets.reset();  // 全新队列状态

    auto demuxer = std::make_unique<Demuxer>();
    auto decoder = std::make_unique<Decoder>();

    // 上游结束/失败：由 Pipeline 关闭队列（队列生命周期只在此处管理），
    // 从而唤醒阻塞在 pop 上的解码线程，让它把残留帧收尾后退出。
    // 注意：此处不置 running=false —— 解码线程可能还在回调残留帧，
    // 交由 decoder 的 finished 回调统一收口。
    demuxer->setStatusCallback([this](std::string message) {
        m_impl->packets.close();
        if (!message.empty() && m_impl->onStatus) {
            m_impl->onStatus(std::move(message));
        }
    });
    decoder->setStatusCallback([this](std::string message) {
        m_impl->running.store(false, std::memory_order_relaxed);
        if (!message.empty() && m_impl->onStatus) {
            m_impl->onStatus(std::move(message));
        }
    });
    // running 只在"解码线程彻底收工"后转 false：
    // 上游上报结束时解码线程可能还在回调残留帧，过早转 false 会让应用层提前收尾。
    decoder->setFinishedCallback([this] { m_impl->running.store(false, std::memory_order_relaxed); });
    if (impl.onFrame) {
        decoder->setFrameCallback(impl.onFrame);
    }

    const auto streams = demuxer->open(url, &impl.packets, error);
    if (!streams) {
        return false;
    }
    if (!decoder->open(streams->video, &impl.packets, error)) {
        return false;
    }

    impl.demuxer = std::move(demuxer);
    impl.decoder = std::move(decoder);

    // 先置运行标志再起线程：避免"线程立刻结束并上报"时被后面这行覆盖回 true。
    impl.running.store(true, std::memory_order_relaxed);
    impl.demuxer->start();
    impl.decoder->start();
    return true;
}

void Pipeline::stop()
{
    Impl& impl = *m_impl;
    impl.running.store(false, std::memory_order_relaxed);

    impl.packets.close();  // 1) 先解阻塞：push/pop 立即返回 false，避免 join 死锁

    if (impl.demuxer) {  // 2) 打断阻塞的 IO 并 join 拉流线程
        impl.demuxer->stop();
        impl.demuxer.reset();
    }
    if (impl.decoder) {  // 3) join 解码线程
        impl.decoder->stop();
        impl.decoder.reset();
    }

    impl.packets.reset();  // 队列复位，供下次 start 复用
}

bool Pipeline::isRunning() const noexcept
{
    return m_impl->running.load(std::memory_order_relaxed);
}

int Pipeline::width() const noexcept
{
    return m_impl->decoder ? m_impl->decoder->width() : 0;
}

int Pipeline::height() const noexcept
{
    return m_impl->decoder ? m_impl->decoder->height() : 0;
}

std::string Pipeline::codecName() const
{
    return m_impl->decoder ? m_impl->decoder->codecName() : std::string{};
}

void Pipeline::setFrameCallback(FrameCallback callback)
{
    m_impl->onFrame = std::move(callback);
}

void Pipeline::setStatusCallback(StatusCallback callback)
{
    m_impl->onStatus = std::move(callback);
}
