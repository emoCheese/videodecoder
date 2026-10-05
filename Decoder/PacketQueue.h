#pragma once

#include "PacketData.h"
#include "RingBuffer.hpp"

/// @file PacketQueue.h
/// @brief 管线内部使用的视频包队列类型。
///
/// 容量 30（约 0.5~1 秒的包量），满时阻塞反压拉流端：
/// 视频包被覆盖丢弃会丢掉参考帧导致花屏，因此这里不能用 OverwriteFullPolicy。
///
/// 内部头文件：仅供 Decoder 库内部使用。

using PacketQueue = RingBuffer<avpacket::Packet, 30, BlockingFullPolicy>;
