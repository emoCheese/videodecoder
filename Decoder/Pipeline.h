#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "PacketData.h"

/// @file Pipeline.h
/// @brief 管线：拉流 + 解码的唯一对外编排入口（公共头文件）。
///
/// 职责边界：
///  - 只做装配与生命周期编排：拥有包队列、决定启停顺序、转发回调；
///  - 不实现拉流/解码细节（分别由 Demuxer / Decoder 负责）；
///  - 内部实现用 pimpl 隐藏，应用层无需也无法接触内部类型。
///
/// 线程约定：
///  - start()/stop() 由应用层（GUI 线程）调用；
///  - FrameCallback / StatusCallback 在库的工作线程上回调：
///    实现内不得阻塞、不得直接触碰 GUI 对象，也不得回调 stop()（会在库线程上 join 自身）；
///  - 应用层必须在销毁回调目标之前调用 stop()。
class Pipeline {
public:
    using FrameCallback = std::function<void(avframe::VideoFramePtr)>;
    using StatusCallback = videopipe::StatusCallback;

    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    /// 打开输入并启动拉流/解码线程。失败时回填可读原因，且不遗留任何线程。
    /// @param url 支持 rtsp://、rtp://（含 SDP 文件路径）与本地文件
    [[nodiscard]] bool start(std::string_view url, std::string* error);

    /// 停止并 join 全部线程（先解阻塞再 join），幂等，可在任意状态调用。
    void stop();

    /// 管线是否仍在工作：拉流/解码任一环节未收工即为 true。
    /// 上游上报"流已结束"后可能仍为 true（解码线程还在回调残留帧），
    /// 转为 false 才表示可以安全收尾。
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] std::string codecName() const;

    /// 回调必须在 start() 之前设置（start() 时会把当前回调装配到内部组件上）。
    void setFrameCallback(FrameCallback callback);
    void setStatusCallback(StatusCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
