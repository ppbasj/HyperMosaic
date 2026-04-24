#include "ffmpeg_raii.h"

namespace hm::ffmpeg {

void FormatContextInputDeleter::operator()(AVFormatContext* context) const noexcept {
    if (context == nullptr) {
        return;
    }

    avformat_close_input(&context);
}

void FormatContextOutputDeleter::operator()(AVFormatContext* context) const noexcept {
    if (context == nullptr) {
        return;
    }

    if (context->pb != nullptr && context->oformat != nullptr && (context->flags & AVFMT_NOFILE) == 0) {
        avio_closep(&context->pb);
    }
    avformat_free_context(context);
}

void CodecContextDeleter::operator()(AVCodecContext* context) const noexcept {
    if (context == nullptr) {
        return;
    }

    avcodec_free_context(&context);
}

void CodecParametersDeleter::operator()(AVCodecParameters* parameters) const noexcept {
    if (parameters == nullptr) {
        return;
    }

    avcodec_parameters_free(&parameters);
}

void FrameDeleter::operator()(AVFrame* frame) const noexcept {
    if (frame == nullptr) {
        return;
    }

    av_frame_free(&frame);
}

void PacketDeleter::operator()(AVPacket* packet) const noexcept {
    if (packet == nullptr) {
        return;
    }

    av_packet_free(&packet);
}

PacketPtr MakePacket() {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        return PacketPtr{};
    }
    return PacketPtr(packet);
}

FramePtr MakeFrame() {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        return FramePtr{};
    }
    return FramePtr(frame);
}

CodecContextPtr MakeCodecContext(const AVCodec* codec) {
    if (codec == nullptr) {
        return CodecContextPtr{};
    }

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        return CodecContextPtr{};
    }
    return CodecContextPtr(context);
}

CodecParametersPtr MakeCodecParameters() {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    if (parameters == nullptr) {
        return CodecParametersPtr{};
    }
    return CodecParametersPtr(parameters);
}

CodecParametersPtr CloneCodecParameters(const AVCodecParameters* source) {
    if (source == nullptr) {
        return CodecParametersPtr{};
    }

    CodecParametersPtr target = MakeCodecParameters();
    if (!target) {
        return CodecParametersPtr{};
    }

    const int result = avcodec_parameters_copy(target.get(), source);
    if (result < 0) {
        return CodecParametersPtr{};
    }

    return target;
}

std::string AvErrorToString(int ffmpegError) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    const int result = av_strerror(ffmpegError, buffer, sizeof(buffer));
    if (result < 0) {
        return "ffmpeg error(" + std::to_string(ffmpegError) + ")";
    }

    return std::string(buffer);
}

}  // namespace hm::ffmpeg
