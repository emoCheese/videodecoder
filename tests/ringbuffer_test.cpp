/// @file ringbuffer_test.cpp
/// @brief RingBuffer 单元测试（只依赖 DecoderPrimitives，不链接 FFmpeg）。
///
/// 覆盖：FIFO 顺序、覆盖策略、阻塞 push/pop、close() 唤醒、关闭后仍可取残留元素、
///       reset() 复用、多生产者/多消费者不丢失不重复。

#include "RingBuffer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
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

/// 等待标志置位（最多 3 秒），避免测试因误判阻塞而永久挂住。
bool waitFor(const std::atomic_bool& flag, std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

void testFifoOrder()
{
    RingBuffer<int, 4> queue;
    for (int i = 0; i < 4; ++i) {
        check(queue.push(i), "入队到未满队列应成功");
    }
    check(queue.full(), "填满后 full() 为真");
    check(queue.size() == 4, "size() 反映元素个数");

    for (int i = 0; i < 4; ++i) {
        int value = -1;
        check(queue.pop(value) && value == i, "FIFO 顺序出队");
    }
    check(queue.empty(), "取空后 empty() 为真");
}

void testOverwritePolicy()
{
    RingBuffer<int, 3, OverwriteFullPolicy> queue;
    for (int i = 0; i < 5; ++i) {
        const bool pushed = queue.push(i);
        check(pushed, "覆盖策略下入队应成功");
    }
    check(queue.size() == 3, "覆盖后容量占用不变");

    std::vector<int> got;
    while (!queue.empty()) {
        int value = 0;
        const bool popped = queue.pop(value);
        if (!popped) {
            break;
        }
        got.push_back(value);
    }
    check(got == std::vector<int>({2, 3, 4}), "覆盖策略丢弃最旧、保留最新");
}

void testBlockingPushWaitsForPop()
{
    RingBuffer<int, 2> queue;
    check(queue.push(1) && queue.push(2), "填满阻塞队列");

    std::atomic_bool pushed{false};
    std::jthread producer([&queue, &pushed] {
        const bool ok = queue.push(3);
        pushed.store(ok);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!pushed.load(), "满队列上的 push 应阻塞");

    int value = 0;
    check(queue.pop(value) && value == 1, "生产者阻塞期间仍可出队");
    check(waitFor(pushed), "pop 之后被阻塞的 push 应被唤醒");
}

void testCloseUnblocksPop()
{
    RingBuffer<int, 2> queue;
    std::atomic_bool woke{false};
    std::atomic_bool popResult{true};

    std::jthread consumer([&queue, &woke, &popResult] {
        int value = 0;
        popResult.store(queue.pop(value));
        woke.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!woke.load(), "空队列上的 pop 应阻塞");

    queue.close();
    check(waitFor(woke), "close() 应唤醒阻塞的 pop");
    check(!popResult.load(), "关闭且为空时 pop 应返回 false");
}

void testCloseUnblocksPush()
{
    RingBuffer<int, 1> queue;
    check(queue.push(1), "填满容量为 1 的队列");

    std::atomic_bool woke{false};
    std::atomic_bool pushResult{true};
    std::jthread producer([&queue, &woke, &pushResult] {
        pushResult.store(queue.push(2));
        woke.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!woke.load(), "满队列上的 push 应阻塞");

    queue.close();
    check(waitFor(woke), "close() 应唤醒阻塞的 push");
    check(!pushResult.load(), "关闭后的 push 应返回 false（元素被丢弃）");
}

void testClosedQueueStillDrains()
{
    RingBuffer<int, 4> queue;
    check(queue.push(7) && queue.push(8), "关闭前入队两个元素");
    queue.close();

    int value = 0;
    check(queue.pop(value) && value == 7, "关闭后仍可取出残留元素（先入）");
    check(queue.pop(value) && value == 8, "关闭后仍可取出残留元素（后入）");
    check(!queue.pop(value), "残留取空后 pop 返回 false");
}

void testResetReuse()
{
    RingBuffer<int, 2> queue;
    check(queue.push(1), "入队一个元素");
    queue.close();
    check(queue.closed(), "closed() 反映关闭状态");

    queue.reset();
    check(!queue.closed(), "reset() 后恢复可用");
    check(queue.empty(), "reset() 后队列为空");
    check(queue.push(2), "reset() 后可继续入队");

    int value = 0;
    check(queue.pop(value) && value == 2, "reset() 后可继续出队");
}

void testMultiProducerConsumer()
{
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 5000;
    constexpr int kTotal = kProducers * kPerProducer;

    RingBuffer<int, 64> queue;
    std::vector<std::jthread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                if (!queue.push(p * kPerProducer + i)) {
                    return;
                }
            }
        });
    }

    std::vector<int> consumed;
    consumed.reserve(kTotal);
    std::mutex consumedMutex;
    std::vector<std::jthread> consumers;
    consumers.reserve(2);
    for (int c = 0; c < 2; ++c) {
        consumers.emplace_back([&queue, &consumed, &consumedMutex] {
            int value = 0;
            while (queue.pop(value)) {
                std::lock_guard lock(consumedMutex);
                consumed.push_back(value);
            }
        });
    }

    producers.clear();  // 生产者自然结束后 join
    queue.close();      // 队列关闭后消费者取空退出
    consumers.clear();  // join 消费者

    check(consumed.size() == static_cast<std::size_t>(kTotal), "多生产者/多消费者：元素无丢失");
    std::sort(consumed.begin(), consumed.end());
    check(std::adjacent_find(consumed.begin(), consumed.end()) == consumed.end(), "多生产者/多消费者：元素无重复");
}

}  // namespace

int main()
{
    testFifoOrder();
    testOverwritePolicy();
    testBlockingPushWaitsForPop();
    testCloseUnblocksPop();
    testCloseUnblocksPush();
    testClosedQueueStillDrains();
    testResetReuse();
    testMultiProducerConsumer();

    if (g_failures == 0) {
        std::printf("\nRingBuffer 全部用例通过\n");
        return 0;
    }
    std::printf("\nRingBuffer 失败用例: %d\n", g_failures);
    return 1;
}
