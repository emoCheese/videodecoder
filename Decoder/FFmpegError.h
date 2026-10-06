#pragma once

#include <format>
extern "C" {
#include <libavutil/error.h>
}

#include <array>
#include <string>

/// @file FfmpegError.h
/// @brief FFmpeg 错误码 → 可读字符串。
///
/// 内部头文件：仅供 Decoder 库内部使用。

namespace ffmpeg_error {

/// 把 FFmpeg 返回的负错误码转换为可读文本。
[[nodiscard]] inline std::string toString(int error)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
        return std::format("未知错误({})", std::to_string(error));
    }
    return std::string(buffer.data());
}

}  // namespace ffmpeg_error
