#pragma once

#include <cstdint>
#include <memory>

/// @file nv12frameview.h
/// @brief 渲染控件的纯数据契约。
///
/// 职责边界：只描述"一帧 NV12 像素的内存布局 + 保活句柄"，
/// 不依赖 FFmpeg、不依赖 Pipeline —— 因此渲染控件不知道帧从哪来，
/// 而 FFmpeg 的 AVFrame 只在应用层的适配器里被解释。

namespace videoframe {

/// 单个平面（可能带行填充，故 stride 与 width 未必相等）。
struct Nv12Plane {
    const std::uint8_t* data = nullptr;
    int stride = 0;  ///< 行字节数
};

/// 一帧 NV12：Y 全尺寸 + UV 交错（宽高各一半）。
struct Nv12FrameView {
    int width = 0;
    int height = 0;
    Nv12Plane y;
    Nv12Plane uv;
    double ptsSeconds = 0.0;

    [[nodiscard]] bool valid() const noexcept
    {
        return width > 0 && height > 0 && (width % 2) == 0 && (height % 2) == 0 && y.data != nullptr &&
               uv.data != nullptr && y.stride >= width && uv.stride >= width / 2;
    }
};

/// 仅用于保活像素内存的不透明句柄：控件不解释其内容，
/// 只保证在持有期间（最迟到下一帧替换）像素内存有效。
using FrameLease = std::shared_ptr<const void>;

}  // namespace videoframe
