#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace hm::ffmpeg {

struct FormatContextInputDeleter {
    void operator()(AVFormatContext* context) const noexcept;
};

struct FormatContextOutputDeleter {
    void operator()(AVFormatContext* context) const noexcept;
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept;
};

struct CodecParametersDeleter {
    void operator()(AVCodecParameters* parameters) const noexcept;
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept;
};

using FormatContextInputPtr = std::unique_ptr<AVFormatContext, FormatContextInputDeleter>;
using FormatContextOutputPtr = std::unique_ptr<AVFormatContext, FormatContextOutputDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using CodecParametersPtr = std::unique_ptr<AVCodecParameters, CodecParametersDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

[[nodiscard]] PacketPtr MakePacket();
[[nodiscard]] FramePtr MakeFrame();
[[nodiscard]] CodecContextPtr MakeCodecContext(const AVCodec* codec);
[[nodiscard]] CodecParametersPtr MakeCodecParameters();
[[nodiscard]] CodecParametersPtr CloneCodecParameters(const AVCodecParameters* source);
[[nodiscard]] std::string AvErrorToString(int ffmpegError);

}  // namespace hm::ffmpeg
