#include "Demuxer.h"

#include <chrono>
#include <utility>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include "FFmpegError.h"

namespace {

/// 选项统一在此构造，便于集中维护与后续扩展（如 UDP 传输开关）。
void applyInputOptions(AVDictionary** options)
{
    // RTSP 默认走 TCP：UDP 在弱网下丢包会直接表现为花屏。
    av_dict_set(options, "rtsp_transport", "tcp", 0);
    // 连接/IO 超时（微秒）。RTSP 与 TCP 均按微秒解释。
    av_dict_set(options, "timeout", "10000000", 0);
    // 允许 SDP 内部引用 rtp/udp 等嵌套协议，否则按 SDP 拉流会被白名单拦下。
    av_dict_set(options, "protocol_whitelist", "file,crypto,data,rtp,udp,tcp,rtsp,http,https", 0);
}

}  // namespace

Demuxer::~Demuxer()
{
    stop();
    close();
}

std::int64_t Demuxer::steadyNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool Demuxer::interrupted() const noexcept
{
    if (m_abort.load(std::memory_order_relaxed)) {
        return true;
    }
    const std::int64_t last = m_lastProgressMs.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    return (steadyNowMs() - last) > m_readTimeout.count();
}

int Demuxer::interruptCallback(void* opaque)
{
    const auto* self = static_cast<const Demuxer*>(opaque);
    if (self == nullptr) {
        return 0;
    }
    return self->interrupted() ? 1 : 0;
}

std::optional<Demuxer::StreamInfo> Demuxer::open(std::string_view url, PacketQueue* queue, std::string* error)
{
    const auto fail = [this, error](std::string message) -> std::optional<StreamInfo> {
        if (error != nullptr) {
            *error = std::move(message);
        }
        close();
        return std::nullopt;
    };

    if (url.empty()) {
        return fail("输入地址为空");
    }
    if (queue == nullptr) {
        return fail("包队列为空");
    }

    stop();   // 确保旧线程已退出
    close();  // 释放上一次的输入资源

    m_url.assign(url);
    m_queue = queue;   // 每次 open 都更新：支持重连后使用新的队列实例
    m_abort.store(false, std::memory_order_relaxed);
    m_lastProgressMs.store(steadyNowMs(), std::memory_order_relaxed);

    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) {
        return fail("avformat_alloc_context 失败");
    }
    // 中断回调必须在 avformat_open_input 之前装好，否则停不下来阻塞中的网络 IO。
    format->interrupt_callback.callback = &Demuxer::interruptCallback;
    format->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    applyInputOptions(&options);

    // 失败时 avformat_open_input 会自行释放 context 并把指针置空，故此处不做清理。
    const int openRet = avformat_open_input(&format, m_url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (openRet < 0) {
        return fail("打开输入失败: " + ffmpeg_error::toString(openRet));
    }

    m_format = format;

    const int infoRet = avformat_find_stream_info(m_format, nullptr);
    if (infoRet < 0) {
        return fail("解析流信息失败: " + ffmpeg_error::toString(infoRet));
    }

    StreamInfo info;
    for (unsigned int i = 0; i < m_format->nb_streams; ++i) {
        AVStream* stream = m_format->streams[i];
        if (stream == nullptr || stream->codecpar == nullptr) {
            continue;
        }
        const AVMediaType type = stream->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && info.video == nullptr) {
            info.video = stream;
            m_videoStreamIndex = static_cast<int>(i);
        }
        else if (type == AVMEDIA_TYPE_AUDIO && info.audio == nullptr) {
            info.audio = stream;
        }
    }

    if (info.video == nullptr || info.video->codecpar == nullptr) {
        return fail("输入中未找到视频流");
    }

    if (m_format->iformat != nullptr && m_format->iformat->name != nullptr) {
        info.formatName = m_format->iformat->name;
    }
    info.codecName = avcodec_get_name(info.video->codecpar->codec_id);
    info.width = info.video->codecpar->width;
    info.height = info.video->codecpar->height;
    return info;
}

void Demuxer::close()
{
    if (m_format != nullptr) {
        avformat_close_input(&m_format);
    }
    m_videoStreamIndex = -1;
}

void Demuxer::start()
{
    m_worker = std::jthread([this](std::stop_token stopToken) { workLoop(stopToken); });
}

void Demuxer::stop()
{
    m_abort.store(true, std::memory_order_relaxed);
    m_worker.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_abort.store(false, std::memory_order_relaxed);
}

void Demuxer::workLoop(std::stop_token stopToken)
{
    std::string endMessage;

    while (!stopToken.stop_requested() && !m_abort.load(std::memory_order_relaxed)) {
        avpacket::PacketPtr packet = avpacket::create();
        if (!packet) {
            // 分配失败按瞬时故障处理：退避后重试，绝不空转打满 CPU。
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const int ret = av_read_frame(m_format, packet.get());
        if (ret == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (ret == AVERROR_EXIT) {
            // 中断回调触发：主动停止时静默退出，否则是长时间无数据。
            if (m_abort.load(std::memory_order_relaxed) || stopToken.stop_requested()) {
                break;
            }
            endMessage = "拉流超时：" + std::to_string(m_readTimeout.count() / 1000) + " 秒内没有收到数据";
            break;
        }
        if (ret == AVERROR_EOF) {
            endMessage = "流已结束";
            break;
        }
        if (ret < 0) {
            endMessage = "拉流失败: " + ffmpeg_error::toString(ret);
            break;
        }

        m_lastProgressMs.store(steadyNowMs(), std::memory_order_relaxed);

        if (packet->stream_index != m_videoStreamIndex) {
            continue;  // 音频等其它流本轮不消费，PacketPtr 离开作用域自动释放
        }

        avpacket::Packet item{std::move(packet), std::chrono::steady_clock::now()};
        if (!m_queue->push(std::move(item))) {
            break;  // 队列已关闭（停止或上游结束）
        }
    }

    if (!endMessage.empty() && m_onStatus) {
        m_onStatus(std::move(endMessage));
    }
}
