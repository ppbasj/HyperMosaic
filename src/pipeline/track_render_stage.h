#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "decode_stage.h"
#include "../tracker/tracker_core.h"

namespace hm::pipeline {

enum class TrackTargetType {
    Face,
    Plate
};

struct TrackRenderStageOptions {
    TrackTargetType targetType{TrackTargetType::Face};
    int detectorInterval{6};
    int faceMosaicBlock{18};
    int plateMosaicBlock{12};
    int maxTrackMissedFrames{12};
    float trackMatchDistance{96.0f};
    std::string faceCascadeModelPath;
    double cascadeScaleFactor{1.1};
    int cascadeMinNeighbors{4};
    int cascadeMinFaceSizePx{36};
};

class TrackRenderStage {
public:
    using FrameWrapperPtr = DecodeThreadPool::FrameWrapperPtr;
    using FrameQueue = DecodeThreadPool::FrameQueue;
    using LogCallback = std::function<void(std::string_view)>;

    TrackRenderStage(
        TrackRenderStageOptions options,
        FrameQueue& inputQueue,
        FrameQueue& outputQueue,
        std::atomic_bool& stopRequested,
        LogCallback infoLog = {},
        LogCallback errorLog = {});

    int Run();

    [[nodiscard]] std::uint64_t rendered_frame_count() const noexcept {
        return renderedFrameCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t detector_invocations() const noexcept {
        return detectorInvocationCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t max_active_track_count() const noexcept {
        return maxActiveTrackCount_.load(std::memory_order_relaxed);
    }

private:
    int ValidateOptions() const;
    void LogInfo(std::string_view message) const;
    void LogError(std::string_view message) const;
    void RenderMosaicOnLuma(AVFrame& frame, const tracker::BoundingBox& box, int blockSize) const;

    TrackRenderStageOptions options_;
    FrameQueue& inputQueue_;
    FrameQueue& outputQueue_;
    std::atomic_bool& stopRequested_;
    LogCallback infoLog_;
    LogCallback errorLog_;

    std::unique_ptr<tracker::IDetector> detector_;
    std::unique_ptr<tracker::TrackManager> trackManager_;

    std::atomic_ullong renderedFrameCount_{0};
    std::atomic_ullong detectorInvocationCount_{0};
    std::atomic_size_t maxActiveTrackCount_{0};
};

}  // namespace hm::pipeline
