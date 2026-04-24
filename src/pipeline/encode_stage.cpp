#include "encode_stage.h"

#include <cerrno>
#include <climits>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hm::pipeline {
namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring widePath = path.native();
    if (widePath.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        widePath.data(),
        static_cast<int>(widePath.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return std::string(widePath.begin(), widePath.end());
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        widePath.data(),
        static_cast<int>(widePath.size()),
        utf8.data(),
        required,
        nullptr,
        nullptr);
    if (converted <= 0) {
        return std::string(widePath.begin(), widePath.end());
    }

    return utf8;
#else
    return path.string();
#endif
}

bool PixelFormatSupported(const AVCodec* encoder, AVPixelFormat format) {
    if (encoder == nullptr) {
        return false;
    }

    if (encoder->pix_fmts == nullptr) {
        return true;
    }

    for (const AVPixelFormat* candidate = encoder->pix_fmts; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate == format) {
            return true;
        }
    }
    return false;
}

AVPixelFormat ChoosePixelFormat(const AVCodec* encoder, AVPixelFormat inputFormat) {
    if (PixelFormatSupported(encoder, inputFormat)) {
        return inputFormat;
    }

    if (encoder != nullptr && encoder->pix_fmts != nullptr && encoder->pix_fmts[0] != AV_PIX_FMT_NONE) {
        return encoder->pix_fmts[0];
    }

    return inputFormat;
}

}  // namespace

EncodeStage::EncodeStage(
    EncodeStageOptions options,
    FrameQueue& inputQueue,
    std::atomic_bool& stopRequested,
    LogCallback infoLog,
    LogCallback errorLog)
    : options_(std::move(options)),
      inputQueue_(inputQueue),
      stopRequested_(stopRequested),
      infoLog_(std::move(infoLog)),
      errorLog_(std::move(errorLog)) {
}

int EncodeStage::ValidateOptions() const {
    if (options_.outputPath.empty()) {
        return AVERROR(EINVAL);
    }

    return 0;
}

int EncodeStage::Run() {
    const int validateResult = ValidateOptions();
    if (validateResult < 0) {
        LogError("Encode options invalid: " + ffmpeg::AvErrorToString(validateResult));
        return validateResult;
    }

    const std::string outputPathUtf8 = PathToUtf8(options_.outputPath);

    ffmpeg::FormatContextOutputPtr outputContext;
    ffmpeg::CodecContextPtr encoderContext;
    AVStream* videoStream = nullptr;
    const AVCodec* encoder = nullptr;
    bool headerWritten = false;
    bool initialized = false;

    std::uint64_t frameCount = 0;
    std::uint64_t packetCount = 0;
    std::uint64_t ptsRollbackCount = 0;
    std::int64_t lastSourcePts = LLONG_MIN;
    std::int64_t lastEncoderPts = LLONG_MIN;
    std::int64_t syntheticPts = 0;
    AVRational sourceTimeBase{1, 25};

    auto writeAvailablePackets = [&]() -> int {
        for (;;) {
            ffmpeg::PacketPtr packet = ffmpeg::MakePacket();
            if (!packet) {
                return AVERROR(ENOMEM);
            }

            const int receiveResult = avcodec_receive_packet(encoderContext.get(), packet.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return 0;
            }
            if (receiveResult < 0) {
                return receiveResult;
            }

            av_packet_rescale_ts(packet.get(), encoderContext->time_base, videoStream->time_base);
            packet->stream_index = videoStream->index;

            const int writeResult = av_interleaved_write_frame(outputContext.get(), packet.get());
            if (writeResult < 0) {
                return writeResult;
            }

            ++packetCount;
        }
    };

    auto initializeEncoder = [&](const AVFrameWrapper& frameWrapper) -> int {
        if (!frameWrapper.frame) {
            return AVERROR(EINVAL);
        }

        AVFrame* firstFrame = frameWrapper.frame.get();
        if (firstFrame->width <= 0 || firstFrame->height <= 0 || firstFrame->format == AV_PIX_FMT_NONE) {
            return AVERROR(EINVAL);
        }

        AVFormatContext* rawOutput = nullptr;
        int result = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, outputPathUtf8.c_str());
        if (result < 0 || rawOutput == nullptr) {
            return (result < 0) ? result : AVERROR(EINVAL);
        }
        outputContext.reset(rawOutput);

        AVCodecID codecId = outputContext->oformat->video_codec;
        if (codecId == AV_CODEC_ID_NONE) {
            codecId = AV_CODEC_ID_H264;
        }

        encoder = avcodec_find_encoder(codecId);
        if (encoder == nullptr) {
            encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        }
        if (encoder == nullptr) {
            return AVERROR_ENCODER_NOT_FOUND;
        }

        videoStream = avformat_new_stream(outputContext.get(), encoder);
        if (videoStream == nullptr) {
            return AVERROR(ENOMEM);
        }

        encoderContext = ffmpeg::MakeCodecContext(encoder);
        if (!encoderContext) {
            return AVERROR(ENOMEM);
        }

        encoderContext->codec_type = AVMEDIA_TYPE_VIDEO;
        encoderContext->width = firstFrame->width;
        encoderContext->height = firstFrame->height;
        encoderContext->sample_aspect_ratio =
            (firstFrame->sample_aspect_ratio.num > 0 && firstFrame->sample_aspect_ratio.den > 0)
                ? firstFrame->sample_aspect_ratio
                : AVRational{1, 1};

        sourceTimeBase =
            (frameWrapper.sourceTimeBase.num > 0 && frameWrapper.sourceTimeBase.den > 0)
                ? frameWrapper.sourceTimeBase
                : AVRational{1, 25};

        if (sourceTimeBase.num <= 0 || sourceTimeBase.den <= 0) {
            sourceTimeBase = AVRational{1, 25};
        }

        encoderContext->time_base = sourceTimeBase;
        encoderContext->framerate = av_inv_q(encoderContext->time_base);
        if (encoderContext->framerate.num <= 0 || encoderContext->framerate.den <= 0) {
            encoderContext->framerate = AVRational{25, 1};
        }

        const AVPixelFormat inputFormat = static_cast<AVPixelFormat>(firstFrame->format);
        const AVPixelFormat chosenFormat = ChoosePixelFormat(encoder, inputFormat);
        if (chosenFormat != inputFormat) {
            LogError(
                "编码器不支持输入像素格式，当前未启用转换，请先使用支持该格式的编码器。input=" +
                std::to_string(static_cast<int>(inputFormat)) +
                ", required=" +
                std::to_string(static_cast<int>(chosenFormat)));
            return AVERROR_PATCHWELCOME;
        }
        encoderContext->pix_fmt = chosenFormat;

        encoderContext->gop_size = 48;
        encoderContext->max_b_frames = 2;
        encoderContext->thread_count = 0;

        if ((outputContext->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            encoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (options_.enableHardware) {
            LogInfo("硬件编码参数已开启，但当前实现使用软件编码器，后续将接入 hw device: " + options_.hwDevice);
        }

        result = avcodec_open2(encoderContext.get(), encoder, nullptr);
        if (result < 0) {
            return result;
        }

        result = avcodec_parameters_from_context(videoStream->codecpar, encoderContext.get());
        if (result < 0) {
            return result;
        }
        videoStream->time_base = encoderContext->time_base;

        if ((outputContext->oformat->flags & AVFMT_NOFILE) == 0) {
            result = avio_open(&outputContext->pb, outputPathUtf8.c_str(), AVIO_FLAG_WRITE);
            if (result < 0) {
                return result;
            }
        }

        result = avformat_write_header(outputContext.get(), nullptr);
        if (result < 0) {
            return result;
        }
        headerWritten = true;
        initialized = true;

        return 0;
    };

    FrameWrapperPtr frameWrapper;
    while (inputQueue_.Pop(frameWrapper, &stopRequested_)) {
        if (!frameWrapper || !frameWrapper->frame) {
            continue;
        }

        if (!initialized) {
            const int initResult = initializeEncoder(*frameWrapper);
            if (initResult < 0) {
                LogError("编码器初始化失败: " + ffmpeg::AvErrorToString(initResult));
                return initResult;
            }
        }

        AVFrame* frame = frameWrapper->frame.get();

        std::int64_t sourcePts = AV_NOPTS_VALUE;
        if (frameWrapper->bestEffortPts != AV_NOPTS_VALUE) {
            sourcePts = frameWrapper->bestEffortPts;
        } else if (frameWrapper->packetPts != AV_NOPTS_VALUE) {
            sourcePts = frameWrapper->packetPts;
        } else if (frame->pts != AV_NOPTS_VALUE) {
            sourcePts = frame->pts;
        }

        if (sourcePts != AV_NOPTS_VALUE) {
            if (lastSourcePts != LLONG_MIN && sourcePts < lastSourcePts) {
                ++ptsRollbackCount;
            }
            lastSourcePts = sourcePts;
        }

        std::int64_t encoderPts = 0;
        if (sourcePts != AV_NOPTS_VALUE) {
            encoderPts = av_rescale_q(sourcePts, sourceTimeBase, encoderContext->time_base);
        } else {
            encoderPts = syntheticPts;
        }

        if (lastEncoderPts != LLONG_MIN && encoderPts <= lastEncoderPts) {
            encoderPts = lastEncoderPts + 1;
        }
        frame->pts = encoderPts;
        lastEncoderPts = encoderPts;
        syntheticPts = encoderPts + 1;

        int sendResult = avcodec_send_frame(encoderContext.get(), frame);
        if (sendResult == AVERROR(EAGAIN)) {
            const int drainResult = writeAvailablePackets();
            if (drainResult < 0) {
                LogError("编码器取包失败: " + ffmpeg::AvErrorToString(drainResult));
                return drainResult;
            }
            sendResult = avcodec_send_frame(encoderContext.get(), frame);
        }
        if (sendResult < 0) {
            LogError("avcodec_send_frame failed: " + ffmpeg::AvErrorToString(sendResult));
            return sendResult;
        }

        const int writeResult = writeAvailablePackets();
        if (writeResult < 0) {
            LogError("写入编码包失败: " + ffmpeg::AvErrorToString(writeResult));
            return writeResult;
        }

        ++frameCount;
        frameWrapper.reset();
    }

    if (initialized) {
        int flushResult = avcodec_send_frame(encoderContext.get(), nullptr);
        if (flushResult != AVERROR_EOF && flushResult < 0) {
            LogError("编码器 flush 失败: " + ffmpeg::AvErrorToString(flushResult));
            return flushResult;
        }

        flushResult = writeAvailablePackets();
        if (flushResult < 0) {
            LogError("编码器 flush 取包失败: " + ffmpeg::AvErrorToString(flushResult));
            return flushResult;
        }

        if (headerWritten) {
            const int trailerResult = av_write_trailer(outputContext.get());
            if (trailerResult < 0) {
                LogError("写入封装尾失败: " + ffmpeg::AvErrorToString(trailerResult));
                return trailerResult;
            }
        }
    }

    encodedFrameCount_.store(frameCount, std::memory_order_relaxed);

    if (stopRequested_.load(std::memory_order_relaxed)) {
        LogInfo("Encode stop requested.");
        return AVERROR_EXIT;
    }

    std::ostringstream oss;
    oss << "Encode completed, frames=" << frameCount
        << ", packets=" << packetCount
        << ", output_path=" << outputPathUtf8
        << ", hw=" << (options_.enableHardware ? "on" : "off")
        << ", hw_device=" << options_.hwDevice
        << ", pts_rollbacks=" << ptsRollbackCount;
    LogInfo(oss.str());

    return 0;
}

void EncodeStage::LogInfo(std::string_view message) const {
    if (infoLog_) {
        infoLog_(message);
    }
}

void EncodeStage::LogError(std::string_view message) const {
    if (errorLog_) {
        errorLog_(message);
    }
}

}  // namespace hm::pipeline
