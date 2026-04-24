#include "tracker_core.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <utility>

#if defined(HYPERMOSAIC_WITH_OPENCV)
#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/objdetect.hpp>
#endif

namespace hm::tracker {
namespace {

#if defined(HYPERMOSAIC_WITH_OPENCV)
std::filesystem::path NormalizePath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }

    ec.clear();
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (!ec) {
        return absolute.lexically_normal();
    }

    return path.lexically_normal();
}

std::optional<std::filesystem::path> ResolveCascadeModelPath(std::string_view configuredPath) {
    std::error_code ec;

    if (!configuredPath.empty()) {
        const auto configured = std::filesystem::path(std::string(configuredPath));
        if (std::filesystem::exists(configured, ec) && !ec) {
            return NormalizePath(configured);
        }
    }

    const std::array<std::filesystem::path, 4> candidates = {
        std::filesystem::path("haarcascade_frontalface_default.xml"),
        std::filesystem::path("data") / "haarcascade_frontalface_default.xml",
        std::filesystem::path("models") / "haarcascade_frontalface_default.xml",
        std::filesystem::path("opencv") / "data" / "haarcascades" / "haarcascade_frontalface_default.xml"};

    for (const auto& candidate : candidates) {
        ec.clear();
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return NormalizePath(candidate);
        }
    }

    const std::string samplePath = cv::samples::findFile(
        "haarcascades/haarcascade_frontalface_default.xml",
        false,
        false);

    if (!samplePath.empty()) {
        const auto discovered = std::filesystem::path(samplePath);
        ec.clear();
        if (std::filesystem::exists(discovered, ec) && !ec) {
            return NormalizePath(discovered);
        }
    }

    return std::nullopt;
}
#endif

}  // namespace

BoundingBox ClampBox(const BoundingBox& box, int frameWidth, int frameHeight) {
    if (frameWidth <= 0 || frameHeight <= 0) {
        return {};
    }

    const float left = std::clamp(box.x, 0.0f, static_cast<float>(frameWidth));
    const float top = std::clamp(box.y, 0.0f, static_cast<float>(frameHeight));
    const float right = std::clamp(box.x + box.width, 0.0f, static_cast<float>(frameWidth));
    const float bottom = std::clamp(box.y + box.height, 0.0f, static_cast<float>(frameHeight));

    return BoundingBox{
        left,
        top,
        std::max(0.0f, right - left),
        std::max(0.0f, bottom - top)};
}

float CenterDistanceSquared(const BoundingBox& lhs, const BoundingBox& rhs) {
    const float lhsCx = lhs.x + lhs.width * 0.5f;
    const float lhsCy = lhs.y + lhs.height * 0.5f;
    const float rhsCx = rhs.x + rhs.width * 0.5f;
    const float rhsCy = rhs.y + rhs.height * 0.5f;

    const float dx = lhsCx - rhsCx;
    const float dy = lhsCy - rhsCy;
    return dx * dx + dy * dy;
}

struct OpenCvCascadeDetector::Impl {
    OpenCvCascadeDetectorOptions options{};
    bool ready{false};
    std::string status{"OpenCV cascade detector not initialized"};

#if defined(HYPERMOSAIC_WITH_OPENCV)
    std::filesystem::path cascadePath;
    cv::CascadeClassifier cascade;
#endif
};

OpenCvCascadeDetector::OpenCvCascadeDetector(OpenCvCascadeDetectorOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->options.scaleFactor = std::max(1.01, impl_->options.scaleFactor);
    impl_->options.minNeighbors = std::max(1, impl_->options.minNeighbors);
    impl_->options.minFaceSizePx = std::max(12, impl_->options.minFaceSizePx);

#if defined(HYPERMOSAIC_WITH_OPENCV)
    const auto resolved = ResolveCascadeModelPath(impl_->options.cascadeModelPath);
    if (!resolved.has_value()) {
        if (impl_->options.cascadeModelPath.empty()) {
            impl_->status =
                "Cascade model not found. Provide --face-cascade <path to haarcascade_frontalface_default.xml>.";
        } else {
            impl_->status = "Configured cascade model not found: " + impl_->options.cascadeModelPath;
        }
        return;
    }

    impl_->cascadePath = *resolved;
    if (!impl_->cascade.load(impl_->cascadePath.string())) {
        impl_->status = "Failed to load cascade model: " + impl_->cascadePath.string();
        return;
    }

    impl_->ready = true;
    impl_->status = "OpenCV Cascade ready: " + impl_->cascadePath.string();
#else
    impl_->status = "OpenCV support is not compiled. Enable OpenCV and rebuild.";
#endif
}

OpenCvCascadeDetector::~OpenCvCascadeDetector() = default;
OpenCvCascadeDetector::OpenCvCascadeDetector(OpenCvCascadeDetector&&) noexcept = default;
OpenCvCascadeDetector& OpenCvCascadeDetector::operator=(OpenCvCascadeDetector&&) noexcept = default;

bool OpenCvCascadeDetector::ready() const noexcept {
    return impl_ && impl_->ready;
}

std::string_view OpenCvCascadeDetector::status_message() const noexcept {
    if (!impl_) {
        return "OpenCV detector impl is not initialized";
    }
    return impl_->status;
}

std::vector<Detection> OpenCvCascadeDetector::Detect(const AVFrame& frame, std::uint64_t frameIndex) {
    (void)frameIndex;

    if (!impl_ || !impl_->ready) {
        return {};
    }

    if (frame.width <= 0 || frame.height <= 0 || frame.data[0] == nullptr || frame.linesize[0] <= 0) {
        return {};
    }

#if !defined(HYPERMOSAIC_WITH_OPENCV)
    return {};
#else
    cv::Mat luma(
        frame.height,
        frame.width,
        CV_8UC1,
        const_cast<std::uint8_t*>(frame.data[0]),
        static_cast<std::size_t>(frame.linesize[0]));

    std::vector<cv::Rect> faceRects;
    impl_->cascade.detectMultiScale(
        luma,
        faceRects,
        impl_->options.scaleFactor,
        impl_->options.minNeighbors,
        0,
        cv::Size(impl_->options.minFaceSizePx, impl_->options.minFaceSizePx));

    std::vector<Detection> detections;
    detections.reserve(faceRects.size());

    for (const auto& rect : faceRects) {
        BoundingBox box;
        box.x = static_cast<float>(rect.x);
        box.y = static_cast<float>(rect.y);
        box.width = static_cast<float>(rect.width);
        box.height = static_cast<float>(rect.height);

        const BoundingBox clamped = ClampBox(box, frame.width, frame.height);
        if (clamped.width < 2.0f || clamped.height < 2.0f) {
            continue;
        }

        detections.push_back(Detection{clamped, 0.92f});
    }

    return detections;
#endif
}

TrackManager::TrackManager(TrackManagerOptions options)
    : options_(options) {
}

void TrackManager::Predict() {
    for (auto& track : tracks_) {
        track.box.x += track.vx;
        track.box.y += track.vy;
        ++track.missedFrames;
        ++track.age;
        track.confidence *= 0.985f;
    }
}

void TrackManager::UpdateVelocityAndBox(TrackState& track, const BoundingBox& newBox) {
    const float oldCx = track.box.x + track.box.width * 0.5f;
    const float oldCy = track.box.y + track.box.height * 0.5f;
    const float newCx = newBox.x + newBox.width * 0.5f;
    const float newCy = newBox.y + newBox.height * 0.5f;

    const float measuredVx = newCx - oldCx;
    const float measuredVy = newCy - oldCy;

    track.vx = track.vx * 0.70f + measuredVx * 0.30f;
    track.vy = track.vy * 0.70f + measuredVy * 0.30f;
    track.box = newBox;
    track.missedFrames = 0;
    track.confidence = std::min(1.0f, track.confidence + 0.12f);
}

int TrackManager::FindNearestTrack(const Detection& detection, const std::vector<bool>& trackMatched) const {
    int nearestTrackIndex = -1;
    float bestDistance = options_.matchDistancePx * options_.matchDistancePx;

    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (trackMatched[i]) {
            continue;
        }

        const float distance = CenterDistanceSquared(tracks_[i].box, detection.box);
        if (distance <= bestDistance) {
            bestDistance = distance;
            nearestTrackIndex = static_cast<int>(i);
        }
    }

    return nearestTrackIndex;
}

void TrackManager::Update(const std::vector<Detection>& detections) {
    std::vector<bool> trackMatched(tracks_.size(), false);

    for (const auto& detection : detections) {
        const int nearestTrackIndex = FindNearestTrack(detection, trackMatched);
        if (nearestTrackIndex >= 0) {
            trackMatched[static_cast<std::size_t>(nearestTrackIndex)] = true;
            UpdateVelocityAndBox(tracks_[static_cast<std::size_t>(nearestTrackIndex)], detection.box);
            continue;
        }

        TrackState track;
        track.id = nextTrackId_++;
        track.box = detection.box;
        track.vx = 0.0f;
        track.vy = 0.0f;
        track.missedFrames = 0;
        track.age = 1;
        track.confidence = std::max(0.45f, detection.score);
        tracks_.push_back(track);
    }

    tracks_.erase(
        std::remove_if(
            tracks_.begin(),
            tracks_.end(),
            [&](const TrackState& track) {
                return track.missedFrames > options_.maxMissedFrames || track.confidence < 0.10f;
            }),
        tracks_.end());
}

std::vector<BoundingBox> TrackManager::ActiveBoxes() const {
    std::vector<BoundingBox> boxes;
    boxes.reserve(tracks_.size());

    for (const auto& track : tracks_) {
        if (track.confidence >= 0.2f) {
            boxes.push_back(track.box);
        }
    }

    return boxes;
}

std::size_t TrackManager::active_track_count() const noexcept {
    return tracks_.size();
}

}  // namespace hm::tracker
