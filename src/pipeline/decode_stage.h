#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "../concurrent/mpsc_queue.h"
#include "../ffmpeg/ffmpeg_raii.h"

namespace hm::pipeline {

struct AVFrameWrapper {
    ffmpeg::FramePtr frame;
    int64_t bestEffortPts{AV_NOPTS_VALUE};
    int64_t packetPts{AV_NOPTS_VALUE};
    uint64_t decodeOrder{0};
    int workerId{-1};
    AVRational sourceTimeBase{1, 1};
};

struct DecodeStageOptions {
    std::size_t workerCount{1};
    std::size_t workerQueueCapacity{64};
    int videoStreamIndex{-1};
    AVRational videoTimeBase{1, 1};
    const AVCodecParameters* codecParameters{nullptr};
};

class DecodeThreadPool {
public:
    using PacketPtr = ffmpeg::PacketPtr;
    using PacketQueue = concurrent::MPSCQueue<PacketPtr>;
    using FrameWrapperPtr = std::shared_ptr<AVFrameWrapper>;
    using FrameQueue = concurrent::MPSCQueue<FrameWrapperPtr>;
    using LogCallback = std::function<void(std::string_view)>;

    DecodeThreadPool(
        DecodeStageOptions options,
        PacketQueue& inputQueue,
        FrameQueue& outputQueue,
        std::atomic_bool& stopRequested,
        LogCallback infoLog = {},
        LogCallback errorLog = {});

    int Run();

private:
    int ValidateOptions() const;
    void LogInfo(std::string_view message) const;
    void LogError(std::string_view message) const;

    DecodeStageOptions options_;
    PacketQueue& inputQueue_;
    FrameQueue& outputQueue_;
    std::atomic_bool& stopRequested_;
    LogCallback infoLog_;
    LogCallback errorLog_;
};

}  // namespace hm::pipeline
