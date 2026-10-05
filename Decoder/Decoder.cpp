#include "Decoder.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

extern "C" {
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include "FfmpegError.h"

Decoder::~Decoder()
{
    stop();
    close();
}

bool Decoder::open(const AVStream* stream, PacketQueue* queue, std::string* error)
{
    const auto fail = [this, error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        close();
        return false;
    };

    if (stream == nullptr || stream->codecpar == nullptr) {
        return fail("视频流为空");
    }
    if (queue == nullptr) {
        return fail("包队列为空");
    }

    stop();
    close();

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return fail(std::string("找不到解码器: ") + avcodec_get_name(stream->codecpar->codec_id));
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (m_codecCtx == nullptr) {
        return fail("avcodec_alloc_context3 失败");
    }

    const int paramsRet = avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    if (paramsRet < 0) {
        return fail("复制解码参数失败: " + ffmpeg_error::toString(paramsRet));
    }

    // 视频解码必须输出所有帧，否则丢帧会破坏参考帧链导致花屏。
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    m_codecCtx->pkt_timebase = stream->time_base;

    const int openRet = avcodec_open2(m_codecCtx, codec, nullptr);
    if (openRet < 0) {
        return fail(std::string("打开解码器失败: ") + ffmpeg_error::toString(openRet));
    }

    m_timeBase = stream->time_base;
    m_width.store(std::max(m_codecCtx->width, 0), std::memory_order_relaxed);
    m_height.store(std::max(m_codecCtx->height, 0), std::memory_order_relaxed);
    m_codecName = (codec->name != nullptr) ? codec->name : avcodec_get_name(codec->id);
    m_queue = queue;
    return true;
}

void Decoder::close()
{
    if (m_codecCtx != nullptr) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_swsCtx != nullptr) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    m_queue = nullptr;
    m_codecName.clear();
}

void Decoder::start()
{
    m_worker = std::jthread([this](std::stop_token stopToken) { workerLoop(stopToken); });
}

void Decoder::stop()
{
    m_worker.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool Decoder::sendPacket(const AVPacket* packet, std::string* error)
{
    // EAGAIN 时"先取帧再重发"，最多重试若干轮，避免实现缺陷导致死循环。
    constexpr int kMaxRetries = 16;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        const int ret = avcodec_send_packet(m_codecCtx, packet);
        if (ret == 0) {
            return true;
        }
        if (ret == AVERROR(EAGAIN)) {
            if (!receiveFrames(error)) {
                return false;
            }
            continue;
        }
        if (ret == AVERROR_EOF) {
            return true;  // 解码器已 flush，重复送包无需处理
        }
        if (error != nullptr) {
            *error = "送入解码器失败: " + ffmpeg_error::toString(ret);
        }
        return false;
    }

    if (error != nullptr) {
        *error = "送入解码器失败: 输出队列持续满载";
    }
    return false;
}

bool Decoder::receiveFrames(std::string* error)
{
    avframe::AVFrameHolder decoded = avframe::create();
    if (!decoded) {
        if (error != nullptr) {
            *error = "av_frame_alloc 失败";
        }
        return false;
    }

    for (;;) {
        const int ret = avcodec_receive_frame(m_codecCtx, decoded.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;  // 需要更多输入 / 已取完
        }
        if (ret < 0) {
            if (error != nullptr) {
                *error = "取出解码帧失败: " + ffmpeg_error::toString(ret);
            }
            return false;
        }

        if (!convertAndEmit(decoded.get(), error)) {
            return false;
        }
        av_frame_unref(decoded.get());  // 复用 holder，避免每帧 alloc/free
    }
}

bool Decoder::convertAndEmit(const AVFrame* source, std::string* error)
{
    if (source->width <= 0 || source->height <= 0) {
        return true;  // 尺寸异常的帧直接跳过
    }

    // NV12 的 UV 平面是宽高各一半，故输出尺寸取偶数（与渲染控件 w/2、h/2 的假设一致）。
    const int dstWidth = source->width & ~1;
    const int dstHeight = source->height & ~1;

    // sws_getCachedContext 在源尺寸/像素格式变化时自动重建，天然支持中途切换分辨率。
    m_swsCtx = sws_getCachedContext(m_swsCtx,
                                   source->width,
                                   source->height,
                                   static_cast<AVPixelFormat>(source->format),
                                   dstWidth,
                                   dstHeight,
                                   AV_PIX_FMT_NV12,
                                   SWS_BILINEAR,
                                   nullptr,
                                   nullptr,
                                   nullptr);
    if (m_swsCtx == nullptr) {
        if (error != nullptr) {
            *error = "创建像素格式转换上下文失败";
        }
        return false;
    }

    avframe::AVFrameHolder converted = avframe::create();
    if (!converted) {
        if (error != nullptr) {
            *error = "av_frame_alloc 失败";
        }
        return false;
    }
    converted->format = AV_PIX_FMT_NV12;
    converted->width = dstWidth;
    converted->height = dstHeight;

    const int bufferRet = av_frame_get_buffer(converted.get(), 32);
    if (bufferRet < 0) {
        if (error != nullptr) {
            *error = "分配输出帧缓冲失败: " + ffmpeg_error::toString(bufferRet);
        }
        return false;
    }

    const int scaled = sws_scale(m_swsCtx,
                                source->data,
                                source->linesize,
                                0,
                                source->height,
                                converted->data,
                                converted->linesize);
    if (scaled <= 0) {
        if (error != nullptr) {
            *error = "像素格式转换失败（sws_scale 返回 " + std::to_string(scaled) + "）";
        }
        return false;
    }

    auto videoFrame = std::make_shared<avframe::VideoFrame>();
    videoFrame->width = dstWidth;
    videoFrame->height = dstHeight;
    videoFrame->format = AV_PIX_FMT_NV12;
    videoFrame->ptsSeconds = framePtsSeconds(source);
    videoFrame->recvAt = m_lastRecvAt;
    videoFrame->decodedAt = std::chrono::steady_clock::now();
    videoFrame->frame = std::move(converted);

    m_width.store(dstWidth, std::memory_order_relaxed);
    m_height.store(dstHeight, std::memory_order_relaxed);

    if (m_onFrame) {
        m_onFrame(std::move(videoFrame));  // 交给应用层：在工作线程上执行，禁止阻塞
    }
    return true;
}

bool Decoder::flush(std::string* error)
{
    if (!sendPacket(nullptr, error)) {
        return false;
    }
    return receiveFrames(error);  // 取到 AVERROR_EOF 为止
}

double Decoder::framePtsSeconds(const AVFrame* frame) const noexcept
{
    std::int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame->pts;
    }
    if (timestamp == AV_NOPTS_VALUE || m_timeBase.den == 0) {
        return 0.0;
    }
    return static_cast<double>(timestamp) * av_q2d(m_timeBase);
}

void Decoder::workerLoop(std::stop_token stopToken)
{
    std::string error;

    while (!stopToken.stop_requested()) {
        avpacket::Packet item;
        if (!m_queue->pop(item)) {
            break;  // 队列已关闭且已取空
        }
        if (!item.pkt) {
            continue;
        }

        m_lastRecvAt = item.recvAt;
        if (!sendPacket(item.pkt.get(), &error)) {
            break;
        }
        if (!receiveFrames(&error)) {
            break;
        }
    }

    // 上游正常结束（而非用户停止）时，把解码器里残留的帧取完再退出。
    if (error.empty() && !stopToken.stop_requested()) {
        flush(&error);
    }

    if (!error.empty() && m_onStatus) {
        m_onStatus(std::move(error));
    }

    // 残留帧与错误都已上报完毕，这里才意味着"解码线程彻底收工"。
    if (m_onFinished) {
        m_onFinished();
    }
}
