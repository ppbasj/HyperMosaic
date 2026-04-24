#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "decode_stage.h"

namespace hm::pipeline {

struct EncodeStageOptions {
    std::filesystem::path outputPath;
    bool enableHardware{false};
    std::string hwDevice{"auto"};
};

class EncodeStage {
public:
    using FrameWrapperPtr = DecodeThreadPool::FrameWrapperPtr;
    using FrameQueue = DecodeThreadPool::FrameQueue;
    using LogCallback = std::function<void(std::string_view)>;

    EncodeStage(
        EncodeStageOptions options,
        FrameQueue& inputQueue,
        std::atomic_bool& stopRequested,
        LogCallback infoLog = {},
        LogCallback errorLog = {});

    int Run();

    [[nodiscard]] std::uint64_t encoded_frame_count() const noexcept {
        return encodedFrameCount_.load(std::memory_order_relaxed);
    }

private:
    int ValidateOptions() const;
    void LogInfo(std::string_view message) const;
    void LogError(std::string_view message) const;

    EncodeStageOptions options_;
    FrameQueue& inputQueue_;
    std::atomic_bool& stopRequested_;
    LogCallback infoLog_;
    LogCallback errorLog_;

    std::atomic_ullong encodedFrameCount_{0};
};

}  // namespace hm::pipeline
