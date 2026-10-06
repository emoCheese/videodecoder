/// @file framequeue_test.cpp
/// @brief FrameQueue 单元测试（应用层组件；只链接 Decoder 以拿到公共 DTO）。
///
/// 覆盖：空队列出队、FIFO 顺序、满队列丢最旧、积压跨度过大时跳帧追赶、
///       清空/计数复位语义、单生产者/单消费者下的计数不变量与出队单调性。

#include "framequeue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (condition) {
        std::printf("[ ok ] %s\n", what.c_str());
    }
    else {
        ++g_failures;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

using Clock = std::chrono::steady_clock;

/// 用 ptsSeconds 当身份标识，便于断言"取到的是哪一帧"。
avframe::VideoFramePtr makeFrame(int id)
{
    auto frame = std::make_shared<avframe::VideoFrame>();
    frame->width = 64;
    frame->height = 48;
    frame->ptsSeconds = static_cast<double>(id);
    return frame;
}

int idOf(const avframe::VideoFramePtr& frame)
{
    return static_cast<int>(frame->ptsSeconds);
}

void testEmptyPop()
{
    FrameQueue queue;
    check(queue.popForDisplay() == nullptr, "空队列出队返回 nullptr");
    check(queue.size() == 0, "空队列 size() 为 0");
    const FrameQueue::Stats stats = queue.stats();
    check(stats.arrived == 0 && stats.overflowDropped == 0 && stats.staleSkipped == 0, "空队列计数全为 0");
    check(queue.capacity() == FrameQueue::kDefaultCapacity, "默认容量为 3");
    check(queue.maxSpan() == FrameQueue::kDefaultMaxSpan, "默认积压跨度上限为 60ms");
}

void testNullFrameIgnored()
{
    FrameQueue queue;
    queue.push(nullptr);
    check(queue.size() == 0 && queue.stats().arrived == 0, "空帧不入队也不计数");
}

void testFifoOrder()
{
    FrameQueue queue;
    const auto t0 = Clock::now();
    queue.push(makeFrame(0), t0);
    queue.push(makeFrame(1), t0 + std::chrono::milliseconds(10));
    queue.push(makeFrame(2), t0 + std::chrono::milliseconds(20));  // 跨度 20ms < 60ms

    check(queue.size() == 3, "三次入队后 size() 为 3");
    for (int expected = 0; expected < 3; ++expected) {
        avframe::VideoFramePtr frame = queue.popForDisplay();
        check(frame != nullptr && idOf(frame) == expected, "严格 FIFO：按到达顺序出队");
    }
    check(queue.popForDisplay() == nullptr, "取空后返回 nullptr");
    const FrameQueue::Stats stats = queue.stats();
    check(stats.arrived == 3 && stats.overflowDropped == 0 && stats.staleSkipped == 0, "正常播放不产生任何丢弃");
}

void testOverflowDropsOldest()
{
    FrameQueue queue;  // 容量 3
    const auto t0 = Clock::now();
    for (int i = 0; i < 4; ++i) {
        queue.push(makeFrame(i), t0 + std::chrono::milliseconds(5 * i));
    }

    const FrameQueue::Stats stats = queue.stats();
    check(stats.arrived == 4, "满队列仍计入到达数");
    check(stats.overflowDropped == 1, "队列满时丢最旧并计入 overflowDropped");
    check(stats.staleSkipped == 0, "队列满不产生过期跳过");
    check(queue.size() == 3, "容量是硬上限");

    avframe::VideoFramePtr first = queue.popForDisplay();
    check(first != nullptr && idOf(first) == 1, "被丢掉的是最旧一帧（剩余从 1 开始）");
}

void testStaleSkipWhenSpanTooLarge()
{
    FrameQueue queue;
    const auto t0 = Clock::now();
    queue.push(makeFrame(0), t0);
    queue.push(makeFrame(1), t0 + std::chrono::milliseconds(20));
    queue.push(makeFrame(2), t0 + std::chrono::milliseconds(70));  // 跨度 70ms > 60ms

    avframe::VideoFramePtr frame = queue.popForDisplay();
    check(frame != nullptr && idOf(frame) == 2, "积压跨度过大：直接取最新帧追赶");
    check(queue.size() == 0, "被跳过的旧帧已释放");
    check(queue.stats().staleSkipped == 2, "跳过的帧计入 staleSkipped");
    check(queue.stats().overflowDropped == 0, "过期跳过不混入 overflowDropped");
}

void testSpanBoundaryKeepsFifo()
{
    FrameQueue queue;
    const auto t0 = Clock::now();
    queue.push(makeFrame(0), t0);
    queue.push(makeFrame(1), t0 + FrameQueue::kDefaultMaxSpan);  // 恰好等于上限：不跳帧

    avframe::VideoFramePtr frame = queue.popForDisplay();
    check(frame != nullptr && idOf(frame) == 0, "跨度等于上限时仍按 FIFO 出队");
    check(queue.stats().staleSkipped == 0, "跨度等于上限不触发过期跳过");
    check(queue.size() == 1, "仅取走队首一帧");
}

void testClearAndResetStats()
{
    FrameQueue queue;
    const auto t0 = Clock::now();
    queue.push(makeFrame(0), t0);
    queue.push(makeFrame(1), t0 + std::chrono::milliseconds(10));

    queue.clear();
    check(queue.size() == 0 && queue.popForDisplay() == nullptr, "clear() 丢弃全部待显示帧");
    check(queue.stats().arrived == 2, "clear() 是生命周期动作，不改写到达计数");
    check(queue.stats().overflowDropped == 0 && queue.stats().staleSkipped == 0, "clear() 不计入丢弃统计");

    queue.push(makeFrame(2), t0);
    queue.resetStats();
    const FrameQueue::Stats stats = queue.stats();
    check(stats.arrived == 0 && stats.overflowDropped == 0 && stats.staleSkipped == 0, "resetStats() 计数归零");
    check(queue.size() == 1, "resetStats() 保留队列内容");
}

/// 单生产者/单消费者：验证计数不变量与"出队序列严格递增"（跳帧与丢帧都不得乱序）。
void testConcurrentInvariants()
{
    constexpr int kTotal = 20000;
    FrameQueue queue;
    std::atomic_bool producerDone{false};
    std::vector<int> consumed;
    consumed.reserve(kTotal);

    std::thread producer([&queue, &producerDone] {
        for (int i = 0; i < kTotal; ++i) {
            queue.push(makeFrame(i));
        }
        producerDone.store(true, std::memory_order_release);
    });

    bool drained = false;
    while (!drained) {
        if (avframe::VideoFramePtr frame = queue.popForDisplay()) {
            consumed.push_back(idOf(frame));
        }
        else if (producerDone.load(std::memory_order_acquire)) {
            drained = true;
        }
        else {
            std::this_thread::yield();
        }
    }
    while (avframe::VideoFramePtr frame = queue.popForDisplay()) {
        consumed.push_back(idOf(frame));
    }
    producer.join();

    const FrameQueue::Stats stats = queue.stats();
    check(stats.arrived == static_cast<unsigned long long>(kTotal), "并发场景：到达计数等于生产总数");
    check(queue.size() == 0, "并发场景：消费结束后队列已取空");
    check(consumed.size() + stats.overflowDropped + stats.staleSkipped ==
              static_cast<std::size_t>(kTotal),
          "并发场景：到达 = 上屏 + 队列满丢弃 + 过期跳过");
    check(std::is_sorted(consumed.begin(), consumed.end()) &&
              std::adjacent_find(consumed.begin(), consumed.end()) == consumed.end(),
          "并发场景：出队序列严格递增（无乱序、无重复）");
    check(stats.staleSkipped == 0, "并发场景：生产远快于消费时只应发生队列满丢弃，不应误判过期");
}

}  // namespace

int main()
{
    testEmptyPop();
    testNullFrameIgnored();
    testFifoOrder();
    testOverflowDropsOldest();
    testStaleSkipWhenSpanTooLarge();
    testSpanBoundaryKeepsFifo();
    testClearAndResetStats();
    testConcurrentInvariants();

    if (g_failures == 0) {
        std::printf("\nFrameQueue 全部用例通过\n");
        return 0;
    }
    std::printf("\nFrameQueue 失败用例: %d\n", g_failures);
    return 1;
}
