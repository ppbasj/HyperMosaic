#include "demux_stage.h"

#include <cerrno>
#include <cstdint>
#include <sstream>
#include <utility>

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

}  // namespace

DemuxStage::DemuxStage(
    DemuxStageOptions options,
    PacketQueue& outputQueue,
    std::atomic_bool& stopRequested,
    LogCallback infoLog,
    LogCallback errorLog)
    : options_(std::move(options)),
      outputQueue_(outputQueue),
      stopRequested_(stopRequested),
      infoLog_(std::move(infoLog)),
      errorLog_(std::move(errorLog)) {
}

int DemuxStage::OpenInput() {
    if (formatContext_) {
        return 0;
    }

    const std::string inputPath = InputPathUtf8();

    AVFormatContext* rawContext = nullptr;
    int result = avformat_open_input(&rawContext, inputPath.c_str(), nullptr, nullptr);
    if (result < 0) {
        LogError("avformat_open_input failed: " + ffmpeg::AvErrorToString(result));
        return result;
    }
    formatContext_.reset(rawContext);

    result = avformat_find_stream_info(formatContext_.get(), nullptr);
    if (result < 0) {
        LogError("avformat_find_stream_info failed: " + ffmpeg::AvErrorToString(result));
        return result;
    }

    videoStreamIndex_ = av_find_best_stream(
        formatContext_.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0);
    if (videoStreamIndex_ < 0) {
        LogError("Cannot find a video stream: " + ffmpeg::AvErrorToString(videoStreamIndex_));
        return videoStreamIndex_;
    }

    const AVStream* videoStream = formatContext_->streams[videoStreamIndex_];
    videoTimeBase_ = videoStream->time_base;

    videoCodecParameters_ = ffmpeg::CloneCodecParameters(videoStream->codecpar);
    if (!videoCodecParameters_) {
        LogError("clone video codec parameters failed");
        return AVERROR(ENOMEM);
    }

    std::ostringstream oss;
    oss << "Demux open success, video_stream_index=" << videoStreamIndex_
        << ", time_base=" << videoTimeBase_.num << '/' << videoTimeBase_.den
        << ", queue_capacity=" << options_.packetQueueCapacity;
    LogInfo(oss.str());

    return 0;
}

int DemuxStage::Prepare() {
    return OpenInput();
}

int DemuxStage::Run() {
    const int openResult = Prepare();
    if (openResult < 0) {
        outputQueue_.Close();
        return openResult;
    }

    std::uint64_t videoPacketCount = 0;

    for (;;) {
        if (stopRequested_.load(std::memory_order_relaxed)) {
            LogInfo("Demux stop requested.");
            outputQueue_.Close();
            return AVERROR_EXIT;
        }

        PacketPtr packet = ffmpeg::MakePacket();
        if (!packet) {
            LogError("av_packet_alloc failed");
            outputQueue_.Close();
            return AVERROR(ENOMEM);
        }

        const int readResult = av_read_frame(formatContext_.get(), packet.get());
        if (readResult == AVERROR_EOF) {
            std::ostringstream oss;
            oss << "Demux reached EOF, video_packets=" << videoPacketCount;
            LogInfo(oss.str());
            outputQueue_.Close();
            return 0;
        }

        if (readResult < 0) {
            LogError("av_read_frame failed: " + ffmpeg::AvErrorToString(readResult));
            outputQueue_.Close();
            return readResult;
        }

        if (packet->stream_index != videoStreamIndex_) {
            continue;
        }

        ++videoPacketCount;
        if (!outputQueue_.Push(std::move(packet), &stopRequested_)) {
            LogInfo("Packet queue closed, demux exits.");
            outputQueue_.Close();
            return stopRequested_.load(std::memory_order_relaxed) ? AVERROR_EXIT : 0;
        }
    }
}

std::string DemuxStage::InputPathUtf8() const {
    return PathToUtf8(options_.inputPath);
}

void DemuxStage::LogInfo(std::string_view message) const {
    if (infoLog_) {
        infoLog_(message);
    }
}

void DemuxStage::LogError(std::string_view message) const {
    if (errorLog_) {
        errorLog_(message);
    }
}

}  // namespace hm::pipeline
