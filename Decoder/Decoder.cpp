#include "Decoder.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>  // av_image_copy_plane
#include <libavutil/pixfmt.h>
}

#include "FFmpegError.h"

namespace {

/// 输出约定：NV12（UV 交错、宽高各半）+ 宽高偶数。
constexpr AVPixelFormat kOutputFormat = AV_PIX_FMT_NV12;
/// 输出帧缓冲对齐：与改造前 sws_scale 的输出保持一致。
constexpr int kOutputAlign = 32;

/// NVIDIA 硬件解码器名：<编码名>_cuvid（如 h264 → h264_cuvid）。
/// 该编码没有对应实现（未编译进来）时返回 nullptr，由调用方回退软解。
[[nodiscard]] const AVCodec* findCuvidDecoder(AVCodecID codecId)
{
    const AVCodecDescriptor* desc = avcodec_descriptor_get(codecId);
    if (desc == nullptr || desc->name == nullptr) {
        return nullptr;
    }
    const std::string hwName = std::format("{}_cuvid", desc->name);
    return avcodec_find_decoder_by_name(hwName.c_str());
}

/// 是否是硬件帧：data[] 指向显存，必须先下载到主机内存再按主机内存解释。
[[nodiscard]] bool isHardwareFrame(const AVFrame* frame) noexcept
{
    if (frame->hw_frames_ctx != nullptr) {
        return true;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    return desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

/// swscale 入口的格式规整：YUVJ*（full range）按对应的 YUV* 处理，与改造前行为一致。
[[nodiscard]] AVPixelFormat swscaleSourceFormat(AVPixelFormat format) noexcept
{
    switch (format) {
        case AV_PIX_FMT_YUVJ420P:
            return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P:
            return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P:
            return AV_PIX_FMT_YUV444P;
        default:
            return format;
    }
}

void setError(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

Decoder::~Decoder()
{
    stop();
    close();
}

AVCodecContext* Decoder::createContext(const AVCodec* codec, const AVStream* stream,
                                       AVHWDeviceType deviceType, std::string* error)
{
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) {
        setError(error, "avcodec_alloc_context3 失败");
        return nullptr;
    }

    const int paramsRet = avcodec_parameters_to_context(ctx, stream->codecpar);
    if (paramsRet < 0) {
        setError(error, "复制解码参数失败: " + ffmpeg_error::toString(paramsRet));
        avcodec_free_context(&ctx);
        return nullptr;
    }

    if (deviceType != AV_HWDEVICE_TYPE_NONE) {
        AVBufferRef* device = nullptr;
        const int deviceRet = av_hwdevice_ctx_create(&device, deviceType, nullptr, nullptr, 0);
        if (deviceRet < 0) {
            setError(error, "创建硬件设备上下文失败: " + ffmpeg_error::toString(deviceRet));
            avcodec_free_context(&ctx);
            return nullptr;
        }
        // 设备归属解码上下文（avcodec_free_context 负责释放），本地引用随即归还。
        ctx->hw_device_ctx = av_buffer_ref(device);
        av_buffer_unref(&device);
        if (ctx->hw_device_ctx == nullptr) {
            setError(error, "绑定硬件设备上下文失败");
            avcodec_free_context(&ctx);
            return nullptr;
        }
    }

    // 视频解码必须输出所有帧，否则丢帧会破坏参考帧链导致花屏。
    ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    ctx->pkt_timebase = stream->time_base;

    const int openRet = avcodec_open2(ctx, codec, nullptr);
    if (openRet < 0) {
        setError(error, std::string("打开解码器 ") + ((codec->name != nullptr) ? codec->name : "?") +
                            " 失败: " + ffmpeg_error::toString(openRet));
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
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

    // ① 优先 NVIDIA 硬件解码：<编码名>_cuvid + CUDA 设备上下文。
    const AVCodec* codec = findCuvidDecoder(stream->codecpar->codec_id);
    std::string hwReason;
    if (codec != nullptr) {
        m_codecCtx = createContext(codec, stream, AV_HWDEVICE_TYPE_CUDA, &hwReason);
    }

    // ② 设备不可用 / 驱动不支持该档次 / 未编译该解码器 → 回退软件解码，
    //    而不是让整条管线启动失败。实际生效的解码器由 codecName() 暴露。
    if (m_codecCtx == nullptr) {
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            return fail(std::string("找不到解码器: ") + avcodec_get_name(stream->codecpar->codec_id));
        }
        std::string swReason;
        m_codecCtx = createContext(codec, stream, AV_HWDEVICE_TYPE_NONE, &swReason);
        if (m_codecCtx == nullptr) {
            std::string message = "打开解码器失败: " + swReason;
            if (!hwReason.empty()) {
                message += "（硬件解码不可用: " + hwReason + "）";
            }
            return fail(std::move(message));
        }
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
    // 解码上下文同时负责释放它持有的硬件设备引用（avcodec_free_context 内部 unref），
    // 因此硬件设备不需要单独的成员，也不存在漏释放的路径。
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
    if (dstWidth < 2 || dstHeight < 2) {
        return true;  // 退化尺寸构不成 NV12，跳过
    }

    // ① 硬件帧（CUDA 等）的 data[] 是设备指针，先下载到主机内存：
    //    直接按主机内存解释它（sws_scale / 纹理上传）只会得到垃圾或纯色画面。
    avframe::AVFrameHolder downloaded;
    const AVFrame* src = source;
    if (isHardwareFrame(source)) {
        downloaded = avframe::create();
        if (!downloaded) {
            setError(error, "av_frame_alloc 失败（硬件帧下载）");
            return false;
        }
        const int transferRet = av_hwframe_transfer_data(downloaded.get(), source, 0);
        if (transferRet < 0) {
            setError(error, "硬件帧下载到内存失败: " + ffmpeg_error::toString(transferRet));
            return false;
        }
        // 下载后的格式由 AVHWFramesContext::sw_format 决定：8bit 4:2:0 → NV12，
        // 10bit → P010，4:4:4 → YUV444P，因此必须按帧的**实际格式**分流，
        // 而不是按"解码器是不是硬件"分流。
        src = downloaded.get();
    }

    const AVPixelFormat srcFormat = static_cast<AVPixelFormat>(src->format);
    avframe::AVFrameHolder converted;

    if (srcFormat == kOutputFormat) {
        // ② 已经是目标格式：绝不能再交给 swscale 做"同格式同尺寸"转换。libswscale 在该
        //    路径走只拷亮度的快速通道（NV12 的 data[2] 为空 → 色度平面被整片跳过），
        //    输出帧的色度保持未初始化的 0；U=V=0 经 YUV→RGB 后红蓝通道被压到 0，
        //    画面只剩随亮度变化的绿色 —— 即"整幅发绿、画面仍在动"。
        if (src->width == dstWidth && src->height == dstHeight) {
            if (src == downloaded.get()) {
                converted = std::move(downloaded);  // 刚下载的帧：直接接管，零拷贝
            } else {
                converted = avframe::create();  // 解码器内部缓冲：只加引用，不拷像素
                if (!converted) {
                    setError(error, "av_frame_alloc 失败（复用解码帧）");
                    return false;
                }
                const int refRet = av_frame_ref(converted.get(), src);
                if (refRet < 0) {
                    setError(error, "引用解码帧失败: " + ffmpeg_error::toString(refRet));
                    return false;
                }
            }
        } else {
            // ③ 仅需把宽高裁成偶数：逐行裁剪拷贝，不做任何重采样。
            converted = avframe::create();
            if (!converted) {
                setError(error, "av_frame_alloc 失败（尺寸裁剪）");
                return false;
            }
            converted->format = kOutputFormat;
            converted->width = dstWidth;
            converted->height = dstHeight;
            const int bufferRet = av_frame_get_buffer(converted.get(), kOutputAlign);
            if (bufferRet < 0) {
                setError(error, "分配输出帧缓冲失败: " + ffmpeg_error::toString(bufferRet));
                return false;
            }
            // NV12 每行字节数：Y 平面 = 宽，UV 交错平面 = 2 × (宽/2) = 宽。
            if (src->data[0] == nullptr || src->data[1] == nullptr || src->linesize[0] < dstWidth ||
                src->linesize[1] < dstWidth) {
                setError(error, "源 NV12 帧缺少色度平面或行宽不足");
                return false;
            }
            av_image_copy_plane(converted->data[0], converted->linesize[0], src->data[0], src->linesize[0],
                                dstWidth, dstHeight);
            av_image_copy_plane(converted->data[1], converted->linesize[1], src->data[1], src->linesize[1],
                                dstWidth, dstHeight / 2);
        }
    } else {
        // ④ 真正的格式转换（YUV420P / YUVJ* / P010 / YUV444P … → NV12）：swscale 此路径正常。
        //    sws_getCachedContext 在源尺寸/像素格式变化时自动重建，天然支持中途切换分辨率。
        m_swsCtx = sws_getCachedContext(m_swsCtx,
                                        src->width,
                                        src->height,
                                        swscaleSourceFormat(srcFormat),
                                        dstWidth,
                                        dstHeight,
                                        kOutputFormat,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr);
        if (m_swsCtx == nullptr) {
            setError(error, "创建像素格式转换上下文失败");
            return false;
        }

        converted = avframe::create();
        if (!converted) {
            setError(error, "av_frame_alloc 失败（像素格式转换）");
            return false;
        }
        converted->format = kOutputFormat;
        converted->width = dstWidth;
        converted->height = dstHeight;
        const int bufferRet = av_frame_get_buffer(converted.get(), kOutputAlign);
        if (bufferRet < 0) {
            setError(error, "分配输出帧缓冲失败: " + ffmpeg_error::toString(bufferRet));
            return false;
        }

        const int scaled =
            sws_scale(m_swsCtx, src->data, src->linesize, 0, src->height, converted->data, converted->linesize);
        if (scaled <= 0) {
            setError(error, "像素格式转换失败（sws_scale 返回 " + std::to_string(scaled) + "）");
            return false;
        }
    }

    // 出口不变量：NV12 必须有色度平面，且 UV 行字节数至少覆盖一整行。缺色度的帧在渲染端
    // 只会显示成纯色，必须拦在这里，而不是跨线程把坏帧交给 UI。
    if (!converted || converted->data[0] == nullptr || converted->data[1] == nullptr ||
        converted->linesize[1] < dstWidth) {
        setError(error, "规整后的 NV12 帧缺少色度平面或行宽不足");
        return false;
    }

    auto videoFrame = std::make_shared<avframe::VideoFrame>();
    videoFrame->width = dstWidth;
    videoFrame->height = dstHeight;
    videoFrame->format = kOutputFormat;
    videoFrame->ptsSeconds = framePtsSeconds(source);  // pts 取原始解码帧（下载产物不带属性）
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
