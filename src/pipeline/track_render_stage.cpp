#include "track_render_stage.h"

#include <algorithm>
#include <cerrno>
#include <sstream>

namespace hm::pipeline {

TrackRenderStage::TrackRenderStage(
    TrackRenderStageOptions options,
    FrameQueue& inputQueue,
    FrameQueue& outputQueue,
    std::atomic_bool& stopRequested,
    LogCallback infoLog,
    LogCallback errorLog)
    : options_(std::move(options)),
      inputQueue_(inputQueue),
      outputQueue_(outputQueue),
      stopRequested_(stopRequested),
      infoLog_(std::move(infoLog)),
      errorLog_(std::move(errorLog)) {
    if (options_.targetType != TrackTargetType::Face) {
        LogError("Only OpenCV face detector is available. Plate target is not supported in current build.");
    } else {
        tracker::OpenCvCascadeDetectorOptions cascadeOptions;
        cascadeOptions.cascadeModelPath = options_.faceCascadeModelPath;
        cascadeOptions.scaleFactor = options_.cascadeScaleFactor;
        cascadeOptions.minNeighbors = options_.cascadeMinNeighbors;
        cascadeOptions.minFaceSizePx = options_.cascadeMinFaceSizePx;

        auto cascadeDetector = std::make_unique<tracker::OpenCvCascadeDetector>(std::move(cascadeOptions));
        if (!cascadeDetector->ready()) {
            LogError(
                "OpenCV CascadeClassifier unavailable: " +
                std::string(cascadeDetector->status_message()));
            detector_.reset();
        } else {
            LogInfo("TrackRender face detector: OpenCV CascadeClassifier");
            detector_ = std::move(cascadeDetector);
        }
    }

    tracker::TrackManagerOptions trackOptions;
    trackOptions.maxMissedFrames = options_.maxTrackMissedFrames;
    trackOptions.matchDistancePx = options_.trackMatchDistance;
    trackManager_ = std::make_unique<tracker::TrackManager>(trackOptions);
}

int TrackRenderStage::ValidateOptions() const {
    if (options_.targetType != TrackTargetType::Face) {
        return AVERROR(ENOSYS);
    }

    if (options_.detectorInterval <= 0 || options_.detectorInterval > 300) {
        return AVERROR(EINVAL);
    }

    if (options_.faceMosaicBlock < 2 || options_.plateMosaicBlock < 2) {
        return AVERROR(EINVAL);
    }

    if (options_.maxTrackMissedFrames < 1 || options_.maxTrackMissedFrames > 300) {
        return AVERROR(EINVAL);
    }

    if (options_.trackMatchDistance < 1.0f || options_.trackMatchDistance > 2000.0f) {
        return AVERROR(EINVAL);
    }

    if (options_.cascadeScaleFactor < 1.01 || options_.cascadeScaleFactor > 2.0) {
        return AVERROR(EINVAL);
    }

    if (options_.cascadeMinNeighbors < 1 || options_.cascadeMinNeighbors > 32) {
        return AVERROR(EINVAL);
    }

    if (options_.cascadeMinFaceSizePx < 12 || options_.cascadeMinFaceSizePx > 2048) {
        return AVERROR(EINVAL);
    }

    if (!detector_ || !trackManager_) {
        return AVERROR(EINVAL);
    }

    return 0;
}

int TrackRenderStage::Run() {
    const int validateResult = ValidateOptions();
    if (validateResult < 0) {
        LogError("TrackRender options invalid: " + ffmpeg::AvErrorToString(validateResult));
        outputQueue_.Close();
        return validateResult;
    }

    std::uint64_t frameIndex = 0;

    FrameWrapperPtr frameWrapper;
    while (inputQueue_.Pop(frameWrapper, &stopRequested_)) {
        if (stopRequested_.load(std::memory_order_relaxed)) {
            break;
        }

        if (!frameWrapper || !frameWrapper->frame) {
            continue;
        }

        AVFrame* frame = frameWrapper->frame.get();
        if (frame->width <= 0 || frame->height <= 0 || frame->data[0] == nullptr) {
            if (!outputQueue_.Push(std::move(frameWrapper), &stopRequested_)) {
                outputQueue_.Close();
                return stopRequested_.load(std::memory_order_relaxed) ? AVERROR_EXIT : AVERROR(EIO);
            }
            ++frameIndex;
            continue;
        }

        const int writableResult = av_frame_make_writable(frame);
        if (writableResult < 0) {
            LogError("av_frame_make_writable failed: " + ffmpeg::AvErrorToString(writableResult));
            outputQueue_.Close();
            return writableResult;
        }

        if ((frameIndex % static_cast<std::uint64_t>(options_.detectorInterval)) == 0) {
            const auto detections = detector_->Detect(*frame, frameIndex);
            trackManager_->Predict();
            trackManager_->Update(detections);
            detectorInvocationCount_.fetch_add(1, std::memory_order_relaxed);
        } else {
            trackManager_->Predict();
        }

        const auto boxes = trackManager_->ActiveBoxes();
        const int blockSize = (options_.targetType == TrackTargetType::Face) ? options_.faceMosaicBlock : options_.plateMosaicBlock;
        for (const auto& box : boxes) {
            const auto clamped = tracker::ClampBox(box, frame->width, frame->height);
            if (clamped.width < 1.0f || clamped.height < 1.0f) {
                continue;
            }
            RenderMosaicOnLuma(*frame, clamped, blockSize);
        }

        const std::size_t activeTrackCount = trackManager_->active_track_count();
        std::size_t expected = maxActiveTrackCount_.load(std::memory_order_relaxed);
        while (activeTrackCount > expected &&
               !maxActiveTrackCount_.compare_exchange_weak(expected, activeTrackCount, std::memory_order_relaxed)) {
        }

        if (!outputQueue_.Push(std::move(frameWrapper), &stopRequested_)) {
            outputQueue_.Close();
            return stopRequested_.load(std::memory_order_relaxed) ? AVERROR_EXIT : AVERROR(EIO);
        }

        renderedFrameCount_.fetch_add(1, std::memory_order_relaxed);
        ++frameIndex;
    }

    outputQueue_.Close();

    if (stopRequested_.load(std::memory_order_relaxed)) {
        LogInfo("TrackRender stop requested.");
        return AVERROR_EXIT;
    }

    std::ostringstream oss;
    oss << "TrackRender completed, frames=" << renderedFrameCount_.load(std::memory_order_relaxed)
        << ", detector_calls=" << detectorInvocationCount_.load(std::memory_order_relaxed)
        << ", max_active_tracks=" << maxActiveTrackCount_.load(std::memory_order_relaxed);
    LogInfo(oss.str());

    return 0;
}

void TrackRenderStage::LogInfo(std::string_view message) const {
    if (infoLog_) {
        infoLog_(message);
    }
}

void TrackRenderStage::LogError(std::string_view message) const {
    if (errorLog_) {
        errorLog_(message);
    }
}

void TrackRenderStage::RenderMosaicOnLuma(AVFrame& frame, const tracker::BoundingBox& box, int blockSize) const {
    if (frame.data[0] == nullptr || frame.linesize[0] <= 0 || blockSize <= 1) {
        return;
    }

    const int x0 = std::max(0, static_cast<int>(box.x));
    const int y0 = std::max(0, static_cast<int>(box.y));
    const int x1 = std::min(frame.width, static_cast<int>(box.x + box.width));
    const int y1 = std::min(frame.height, static_cast<int>(box.y + box.height));

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    for (int y = y0; y < y1; y += blockSize) {
        const int blockY1 = std::min(y + blockSize, y1);
        for (int x = x0; x < x1; x += blockSize) {
            const int blockX1 = std::min(x + blockSize, x1);

            int sum = 0;
            int count = 0;
            for (int yy = y; yy < blockY1; ++yy) {
                const uint8_t* row = frame.data[0] + yy * frame.linesize[0];
                for (int xx = x; xx < blockX1; ++xx) {
                    sum += row[xx];
                    ++count;
                }
            }

            if (count == 0) {
                continue;
            }

            const uint8_t avg = static_cast<uint8_t>(sum / count);
            for (int yy = y; yy < blockY1; ++yy) {
                uint8_t* row = frame.data[0] + yy * frame.linesize[0];
                for (int xx = x; xx < blockX1; ++xx) {
                    row[xx] = avg;
                }
            }
        }
    }
}

}  // namespace hm::pipeline
