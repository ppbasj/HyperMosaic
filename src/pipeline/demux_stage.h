#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "../concurrent/mpsc_queue.h"
#include "../ffmpeg/ffmpeg_raii.h"

namespace hm::pipeline {

struct DemuxStageOptions {
    std::filesystem::path inputPath;
    std::size_t packetQueueCapacity{512};
};

class DemuxStage {
public:
    using PacketPtr = ffmpeg::PacketPtr;
    using PacketQueue = concurrent::MPSCQueue<PacketPtr>;
    using LogCallback = std::function<void(std::string_view)>;

    DemuxStage(
        DemuxStageOptions options,
        PacketQueue& outputQueue,
        std::atomic_bool& stopRequested,
        LogCallback infoLog = {},
        LogCallback errorLog = {});

    int Prepare();
    int Run();

    [[nodiscard]] int video_stream_index() const noexcept {
        return videoStreamIndex_;
    }

    [[nodiscard]] AVRational video_time_base() const noexcept {
        return videoTimeBase_;
    }

    [[nodiscard]] const AVCodecParameters* video_codec_parameters() const noexcept {
        return videoCodecParameters_.get();
    }

private:
    int OpenInput();
    void LogInfo(std::string_view message) const;
    void LogError(std::string_view message) const;
    [[nodiscard]] std::string InputPathUtf8() const;

    DemuxStageOptions options_;
    PacketQueue& outputQueue_;
    std::atomic_bool& stopRequested_;
    LogCallback infoLog_;
    LogCallback errorLog_;

    ffmpeg::FormatContextInputPtr formatContext_;
    ffmpeg::CodecParametersPtr videoCodecParameters_;
    int videoStreamIndex_{-1};
    AVRational videoTimeBase_{1, 1};
};

}  // namespace hm::pipeline
