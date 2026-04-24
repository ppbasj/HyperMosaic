#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace hm::tracker {

struct BoundingBox {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

[[nodiscard]] BoundingBox ClampBox(const BoundingBox& box, int frameWidth, int frameHeight);
[[nodiscard]] float CenterDistanceSquared(const BoundingBox& lhs, const BoundingBox& rhs);

struct Detection {
    BoundingBox box;
    float score{1.0f};
};

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::vector<Detection> Detect(const AVFrame& frame, std::uint64_t frameIndex) = 0;
};

struct OpenCvCascadeDetectorOptions {
    std::string cascadeModelPath;
    double scaleFactor{1.1};
    int minNeighbors{4};
    int minFaceSizePx{36};
};

class OpenCvCascadeDetector final : public IDetector {
public:
    explicit OpenCvCascadeDetector(OpenCvCascadeDetectorOptions options = {});
    ~OpenCvCascadeDetector() override;

    OpenCvCascadeDetector(OpenCvCascadeDetector&&) noexcept;
    OpenCvCascadeDetector& operator=(OpenCvCascadeDetector&&) noexcept;

    OpenCvCascadeDetector(const OpenCvCascadeDetector&) = delete;
    OpenCvCascadeDetector& operator=(const OpenCvCascadeDetector&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view status_message() const noexcept;

    std::vector<Detection> Detect(const AVFrame& frame, std::uint64_t frameIndex) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct TrackState {
    int id{-1};
    BoundingBox box;
    float vx{0.0f};
    float vy{0.0f};
    int missedFrames{0};
    int age{0};
    float confidence{0.5f};
};

struct TrackManagerOptions {
    int maxMissedFrames{12};
    float matchDistancePx{96.0f};
};

class TrackManager {
public:
    explicit TrackManager(TrackManagerOptions options = {});

    void Predict();
    void Update(const std::vector<Detection>& detections);

    [[nodiscard]] std::vector<BoundingBox> ActiveBoxes() const;
    [[nodiscard]] std::size_t active_track_count() const noexcept;

private:
    static void UpdateVelocityAndBox(TrackState& track, const BoundingBox& newBox);
    [[nodiscard]] int FindNearestTrack(const Detection& detection, const std::vector<bool>& trackMatched) const;

    TrackManagerOptions options_;
    std::vector<TrackState> tracks_;
    int nextTrackId_{1};
};

}  // namespace hm::tracker
