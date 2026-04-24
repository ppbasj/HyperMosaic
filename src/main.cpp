#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "ffmpeg/ffmpeg_raii.h"
#include "pipeline/decode_stage.h"
#include "pipeline/demux_stage.h"
#include "pipeline/encode_stage.h"
#include "pipeline/track_render_stage.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hm {

enum class TargetType {
    Face,
    Plate
};

#ifdef _WIN32
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required <= 0) {
        return std::string(wide.begin(), wide.end());
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        result.data(),
        required,
        nullptr,
        nullptr);

    if (converted <= 0) {
        return std::string(wide.begin(), wide.end());
    }

    return result;
}
#endif

[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view raw) {
    return std::filesystem::u8path(raw.begin(), raw.end());
}

[[nodiscard]] std::string PathToDisplayString(const std::filesystem::path& path) {
#ifdef _WIN32
    return WideToUtf8(path.native());
#else
    return path.string();
#endif
}

[[nodiscard]] std::optional<TargetType> ParseTargetType(std::string_view raw) {
    std::string normalized(raw);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (normalized == "face" || normalized == "faces" || normalized == "人脸") {
        return TargetType::Face;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view ToString(TargetType type) {
    switch (type) {
    case TargetType::Face:
        return "face";
    case TargetType::Plate:
        return "plate";
    }
    return "unknown";
}

struct AppConfig {
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    TargetType targetType{TargetType::Face};

    std::size_t decodeWorkers{1};
    std::size_t packetQueueCapacity{512};
    std::size_t frameQueueCapacity{256};
    int detectorInterval{6};
    std::filesystem::path faceCascadeModelPath;

    bool enableHwEncode{false};
    std::string hwDevice{"auto"};
    bool dryRun{false};
};

enum class ParseStatus {
    Ok,
    Help,
    Error
};

struct ParseResult {
    ParseStatus status{ParseStatus::Error};
    AppConfig config{};
    std::string message;
};

template <typename T>
[[nodiscard]] std::optional<T> ParseInteger(std::string_view raw) {
    T value{};
    const auto* begin = raw.data();
    const auto* end = raw.data() + raw.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

class CliParser {
public:
    static ParseResult Parse(int argc, const char* const argv[]) {
        ParseResult result;
        result.status = ParseStatus::Ok;

        if (argc <= 1) {
            result.status = ParseStatus::Help;
            return result;
        }

        bool hasInput = false;
        bool hasTarget = false;
        bool optionsEnded = false;
        int positionalIndex = 0;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (!optionsEnded && arg == "--") {
                optionsEnded = true;
                continue;
            }

            if (!optionsEnded && (arg == "-h" || arg == "--help")) {
                result.status = ParseStatus::Help;
                return result;
            }

            if (!optionsEnded && !arg.empty() && arg[0] == '-') {
                if (arg == "--input") {
                    if (i + 1 >= argc) {
                        return Error("--input 缺少参数值");
                    }
                    result.config.inputPath = PathFromUtf8(argv[++i]);
                    hasInput = true;
                    continue;
                }
                if (arg == "--output") {
                    if (i + 1 >= argc) {
                        return Error("--output 缺少参数值");
                    }
                    result.config.outputPath = PathFromUtf8(argv[++i]);
                    continue;
                }
                if (arg == "--target") {
                    if (i + 1 >= argc) {
                        return Error("--target 缺少参数值");
                    }
                    const auto parsed = ParseTargetType(argv[++i]);
                    if (!parsed.has_value()) {
                        return Error("--target 仅支持 face（OpenCV）");
                    }
                    result.config.targetType = *parsed;
                    hasTarget = true;
                    continue;
                }
                if (arg == "--decode-workers") {
                    if (i + 1 >= argc) {
                        return Error("--decode-workers 缺少参数值");
                    }
                    const auto parsed = ParseInteger<std::size_t>(argv[++i]);
                    if (!parsed.has_value() || *parsed == 0 || *parsed > 256) {
                        return Error("--decode-workers 需在 1-256 范围内");
                    }
                    result.config.decodeWorkers = *parsed;
                    continue;
                }
                if (arg == "--packet-queue") {
                    if (i + 1 >= argc) {
                        return Error("--packet-queue 缺少参数值");
                    }
                    const auto parsed = ParseInteger<std::size_t>(argv[++i]);
                    if (!parsed.has_value() || *parsed < 64 || *parsed > 8192) {
                        return Error("--packet-queue 需在 64-8192 范围内");
                    }
                    result.config.packetQueueCapacity = *parsed;
                    continue;
                }
                if (arg == "--frame-queue") {
                    if (i + 1 >= argc) {
                        return Error("--frame-queue 缺少参数值");
                    }
                    const auto parsed = ParseInteger<std::size_t>(argv[++i]);
                    if (!parsed.has_value() || *parsed < 32 || *parsed > 8192) {
                        return Error("--frame-queue 需在 32-8192 范围内");
                    }
                    result.config.frameQueueCapacity = *parsed;
                    continue;
                }
                if (arg == "--detector-interval") {
                    if (i + 1 >= argc) {
                        return Error("--detector-interval 缺少参数值");
                    }
                    const auto parsed = ParseInteger<int>(argv[++i]);
                    if (!parsed.has_value() || *parsed < 1 || *parsed > 240) {
                        return Error("--detector-interval 需在 1-240 范围内");
                    }
                    result.config.detectorInterval = *parsed;
                    continue;
                }
                if (arg == "--face-cascade") {
                    if (i + 1 >= argc) {
                        return Error("--face-cascade 缺少参数值");
                    }
                    result.config.faceCascadeModelPath = PathFromUtf8(argv[++i]);
                    continue;
                }
                if (arg == "--hw-encode") {
                    result.config.enableHwEncode = true;
                    continue;
                }
                if (arg == "--no-hw-encode") {
                    result.config.enableHwEncode = false;
                    continue;
                }
                if (arg == "--hw-device") {
                    if (i + 1 >= argc) {
                        return Error("--hw-device 缺少参数值");
                    }
                    result.config.hwDevice = argv[++i];
                    result.config.enableHwEncode = true;
                    continue;
                }
                if (arg == "--dry-run") {
                    result.config.dryRun = true;
                    continue;
                }

                return Error("未知参数: " + arg);
            }

            if (positionalIndex == 0) {
                result.config.inputPath = PathFromUtf8(arg);
                hasInput = true;
            } else if (positionalIndex == 1) {
                const auto parsed = ParseTargetType(arg);
                if (!parsed.has_value()) {
                    return Error("位置参数 target 仅支持 face（OpenCV）");
                }
                result.config.targetType = *parsed;
                hasTarget = true;
            } else if (positionalIndex == 2) {
                result.config.outputPath = PathFromUtf8(arg);
            } else {
                return Error("参数过多，请使用 --help 查看用法");
            }
            ++positionalIndex;
        }

        if (!hasInput) {
            return Error("缺少输入视频路径，请使用 --input 或位置参数提供");
        }
        if (!hasTarget) {
            return Error("缺少 target 类型，请使用 --target 或第二个位置参数提供");
        }

        if (result.config.outputPath.empty()) {
            const auto stem = result.config.inputPath.stem();
            std::filesystem::path outputName;

            if (stem.empty()) {
                outputName = "output_mosaic.mp4";
            } else {
                outputName = stem;
                outputName += "_mosaic";

                const auto ext = result.config.inputPath.extension();
                if (ext.empty()) {
                    outputName += ".mp4";
                } else {
                    outputName += ext;
                }
            }

            result.config.outputPath = result.config.inputPath.parent_path() / outputName;
        }

        return result;
    }

    static void PrintHelp(std::ostream& os, std::string_view exeName) {
        os
            << "HyperMosaic - 实时视频马赛克追踪渲染引擎\n\n"
            << "用法:\n"
            << "  " << exeName << " --input <video> --target <face> [--output <video>] [options]\n"
            << "  " << exeName << " <input> <face> [output]\n\n"
            << "必选参数:\n"
            << "  --input <path>            输入视频路径\n"
            << "  --target <face>           打码目标类型（OpenCV 人脸）\n\n"
            << "可选参数:\n"
            << "  --output <path>           输出视频路径，默认 input_stem_mosaic.mp4\n"
            << "  --decode-workers <n>      解码线程数，默认 1（稳定模式）\n"
            << "  --packet-queue <n>        Demux->Decode 队列容量，默认 512\n"
            << "  --frame-queue <n>         Decode->Track 队列容量，默认 256\n"
            << "  --detector-interval <n>   检测间隔帧，默认 6\n"
            << "  --face-cascade <path>     OpenCV Cascade 人脸模型 xml 路径\n"
            << "  --hw-encode               启用硬件编码\n"
            << "  --no-hw-encode            显式关闭硬件编码\n"
            << "  --hw-device <name>        指定硬件设备，如 d3d11va\n"
            << "  --dry-run                 仅验证配置与流水线装配，不执行实际编解码\n"
            << "  --                        选项结束，后续参数按位置参数处理\n"
            << "  -h, --help                打印帮助\n";
    }

private:
    static ParseResult Error(std::string message) {
        ParseResult result;
        result.status = ParseStatus::Error;
        result.message = std::move(message);
        return result;
    }
};

class ConsoleLogger {
public:
    enum class Level {
        Info,
        Warn,
        Error
    };

    void Info(std::string_view message) {
        Write(Level::Info, message);
    }

    void Warn(std::string_view message) {
        Write(Level::Warn, message);
    }

    void Error(std::string_view message) {
        Write(Level::Error, message);
    }

private:
    [[nodiscard]] static std::string Now() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%H:%M:%S");
        return oss.str();
    }

    [[nodiscard]] static std::string_view ToLabel(Level level) {
        switch (level) {
        case Level::Info:
            return "INFO";
        case Level::Warn:
            return "WARN";
        case Level::Error:
            return "ERROR";
        }
        return "UNKNOWN";
    }

    void Write(Level level, std::string_view message) {
        std::scoped_lock lock(mutex_);
        std::cout << "[" << Now() << "] [" << ToLabel(level) << "] " << message << '\n';
    }

    std::mutex mutex_;
};

struct DemuxOptions {
    std::filesystem::path inputPath;
    std::size_t packetQueueCapacity{512};
};

struct DecodeOptions {
    std::size_t workerCount{1};
};

struct TrackRenderOptions {
    TargetType targetType{TargetType::Face};
    int detectorInterval{6};
    std::size_t frameQueueCapacity{256};
    std::filesystem::path faceCascadeModelPath;
};

struct EncodeOptions {
    std::filesystem::path outputPath;
    bool enableHardware{false};
    std::string hwDevice{"auto"};
};

struct PipelineOptions {
    DemuxOptions demux;
    DecodeOptions decode;
    TrackRenderOptions trackRender;
    EncodeOptions encode;
    bool dryRun{false};
};

[[nodiscard]] std::filesystem::path ToComparablePath(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }

    ec.clear();
    auto absolute = std::filesystem::absolute(path, ec);
    if (!ec) {
        return absolute.lexically_normal();
    }

    return path.lexically_normal();
}

[[nodiscard]] bool IsSamePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
#ifdef _WIN32
    std::wstring left = ToComparablePath(lhs).native();
    std::wstring right = ToComparablePath(rhs).native();
    std::transform(left.begin(), left.end(), left.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    std::transform(right.begin(), right.end(), right.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return left == right;
#else
    return ToComparablePath(lhs) == ToComparablePath(rhs);
#endif
}

[[nodiscard]] bool ValidateAndPrepareConfig(AppConfig& config, std::string& errorMessage) {
    std::error_code ec;

    if (!std::filesystem::exists(config.inputPath, ec)) {
        if (ec) {
            errorMessage = "检查输入路径失败: " + ec.message();
        } else {
            errorMessage = "输入文件不存在: " + PathToDisplayString(config.inputPath);
        }
        return false;
    }

    ec.clear();
    if (!std::filesystem::is_regular_file(config.inputPath, ec)) {
        if (ec) {
            errorMessage = "检查输入文件类型失败: " + ec.message();
        } else {
            errorMessage = "输入路径不是文件: " + PathToDisplayString(config.inputPath);
        }
        return false;
    }

    ec.clear();
    const bool outputExists = std::filesystem::exists(config.outputPath, ec);
    if (ec) {
        errorMessage = "检查输出路径失败: " + ec.message();
        return false;
    }

    if (outputExists) {
        ec.clear();
        if (std::filesystem::is_directory(config.outputPath, ec)) {
            if (ec) {
                errorMessage = "检查输出路径类型失败: " + ec.message();
            } else {
                errorMessage = "输出路径指向目录而非文件: " + PathToDisplayString(config.outputPath);
            }
            return false;
        }
    }

    auto outputParent = config.outputPath.parent_path();
    if (outputParent.empty()) {
        outputParent = ".";
    }

    ec.clear();
    const bool parentExists = std::filesystem::exists(outputParent, ec);
    if (ec) {
        errorMessage = "检查输出目录失败: " + ec.message();
        return false;
    }

    if (!parentExists) {
        std::filesystem::create_directories(outputParent, ec);
        if (ec) {
            errorMessage = "创建输出目录失败: " + PathToDisplayString(outputParent) + " - " + ec.message();
            return false;
        }
    } else {
        ec.clear();
        if (!std::filesystem::is_directory(outputParent, ec)) {
            if (ec) {
                errorMessage = "检查输出目录类型失败: " + ec.message();
            } else {
                errorMessage = "输出父路径不是目录: " + PathToDisplayString(outputParent);
            }
            return false;
        }
    }

    if (IsSamePath(config.inputPath, config.outputPath)) {
        errorMessage = "输入与输出不能是同一路径: " + PathToDisplayString(config.inputPath);
        return false;
    }

    return true;
}

[[nodiscard]] PipelineOptions BuildPipelineOptions(const AppConfig& app) {
    PipelineOptions options;
    options.demux.inputPath = app.inputPath;
    options.demux.packetQueueCapacity = app.packetQueueCapacity;

    options.decode.workerCount = app.decodeWorkers;

    options.trackRender.targetType = app.targetType;
    options.trackRender.detectorInterval = app.detectorInterval;
    options.trackRender.frameQueueCapacity = app.frameQueueCapacity;
    options.trackRender.faceCascadeModelPath = app.faceCascadeModelPath;

    options.encode.outputPath = app.outputPath;
    options.encode.enableHardware = app.enableHwEncode;
    options.encode.hwDevice = app.hwDevice;

    options.dryRun = app.dryRun;
    return options;
}

class IPipeline {
public:
    virtual ~IPipeline() = default;
    virtual bool Start() = 0;
    virtual int Wait() = 0;
    virtual void RequestStop() noexcept = 0;
};

class BootstrapPipeline final : public IPipeline {
public:
    BootstrapPipeline(PipelineOptions options, ConsoleLogger& logger, std::atomic_bool& stopRequested)
        : options_(std::move(options)),
          logger_(logger),
          stopRequested_(stopRequested) {
    }

    ~BootstrapPipeline() override {
        RequestStop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool Start() override {
        if (started_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        worker_ = std::thread([this]() {
            Run();
        });
        return true;
    }

    int Wait() override {
        if (worker_.joinable()) {
            worker_.join();
        }
        return exitCode_;
    }

    void RequestStop() noexcept override {
        stopRequested_.store(true, std::memory_order_relaxed);
    }

private:
    void Run() {
        logger_.Info("Pipeline bootstrap started.");
        LogConfiguration();
        LogDataFlow();

        if (options_.dryRun) {
            logger_.Info("Dry-run 模式：跳过 Demux 执行，仅验证配置与流水线装配。");
            logger_.Info("Bootstrap pipeline finished.");
            exitCode_ = 0;
            return;
        }

        pipeline::DemuxStage::PacketQueue packetQueue(options_.demux.packetQueueCapacity);
        pipeline::DecodeThreadPool::FrameQueue decodedFrameQueue(options_.trackRender.frameQueueCapacity);
        pipeline::TrackRenderStage::FrameQueue renderedFrameQueue(options_.trackRender.frameQueueCapacity);

        pipeline::DemuxStageOptions demuxOptions;
        demuxOptions.inputPath = options_.demux.inputPath;
        demuxOptions.packetQueueCapacity = options_.demux.packetQueueCapacity;

        pipeline::DemuxStage demuxStage(
            std::move(demuxOptions),
            packetQueue,
            stopRequested_,
            [this](std::string_view message) {
                logger_.Info("[Demux] " + std::string(message));
            },
            [this](std::string_view message) {
                logger_.Error("[Demux] " + std::string(message));
            });

        const int demuxPrepareResult = demuxStage.Prepare();
        if (demuxPrepareResult < 0) {
            logger_.Error("Demux 预处理失败: " + ffmpeg::AvErrorToString(demuxPrepareResult));
            exitCode_ = 5;
            return;
        }

        pipeline::DecodeStageOptions decodeOptions;
        decodeOptions.workerCount = options_.decode.workerCount;
        decodeOptions.workerQueueCapacity = std::max<std::size_t>(32, options_.demux.packetQueueCapacity / std::max<std::size_t>(1, options_.decode.workerCount));
        decodeOptions.videoStreamIndex = demuxStage.video_stream_index();
        decodeOptions.videoTimeBase = demuxStage.video_time_base();
        decodeOptions.codecParameters = demuxStage.video_codec_parameters();

        pipeline::DecodeThreadPool decodeStage(
            std::move(decodeOptions),
            packetQueue,
            decodedFrameQueue,
            stopRequested_,
            [this](std::string_view message) {
                logger_.Info("[Decode] " + std::string(message));
            },
            [this](std::string_view message) {
                logger_.Error("[Decode] " + std::string(message));
            });

        pipeline::TrackRenderStageOptions trackRenderOptions;
        trackRenderOptions.targetType =
            options_.trackRender.targetType == TargetType::Face
                ? pipeline::TrackTargetType::Face
                : pipeline::TrackTargetType::Plate;
        trackRenderOptions.detectorInterval = options_.trackRender.detectorInterval;
        trackRenderOptions.faceCascadeModelPath = PathToDisplayString(options_.trackRender.faceCascadeModelPath);

        pipeline::TrackRenderStage trackRenderStage(
            std::move(trackRenderOptions),
            decodedFrameQueue,
            renderedFrameQueue,
            stopRequested_,
            [this](std::string_view message) {
                logger_.Info("[TrackRender] " + std::string(message));
            },
            [this](std::string_view message) {
                logger_.Error("[TrackRender] " + std::string(message));
            });

        pipeline::EncodeStageOptions encodeOptions;
        encodeOptions.outputPath = options_.encode.outputPath;
        encodeOptions.enableHardware = options_.encode.enableHardware;
        encodeOptions.hwDevice = options_.encode.hwDevice;

        pipeline::EncodeStage encodeStage(
            std::move(encodeOptions),
            renderedFrameQueue,
            stopRequested_,
            [this](std::string_view message) {
                logger_.Info("[Encode] " + std::string(message));
            },
            [this](std::string_view message) {
                logger_.Error("[Encode] " + std::string(message));
            });

        std::atomic_int demuxResult{0};
        std::atomic_int decodeResult{0};
        std::atomic_int trackRenderResult{0};
        std::atomic_int encodeResult{0};

        std::thread demuxThread([&]() {
            demuxResult.store(demuxStage.Run(), std::memory_order_relaxed);
            packetQueue.Close();
        });

        std::thread decodeThread([&]() {
            decodeResult.store(decodeStage.Run(), std::memory_order_relaxed);
            decodedFrameQueue.Close();
        });

        std::thread trackRenderThread([&]() {
            trackRenderResult.store(trackRenderStage.Run(), std::memory_order_relaxed);
            renderedFrameQueue.Close();
        });

        std::thread encodeThread([&]() {
            encodeResult.store(encodeStage.Run(), std::memory_order_relaxed);
        });

        if (demuxThread.joinable()) {
            demuxThread.join();
        }

        packetQueue.Close();

        if (decodeThread.joinable()) {
            decodeThread.join();
        }

        decodedFrameQueue.Close();

        if (trackRenderThread.joinable()) {
            trackRenderThread.join();
        }

        renderedFrameQueue.Close();

        if (encodeThread.joinable()) {
            encodeThread.join();
        }

        const int demuxStageResult = demuxResult.load(std::memory_order_relaxed);
        const int decodeStageResult = decodeResult.load(std::memory_order_relaxed);
        const int trackRenderStageResult = trackRenderResult.load(std::memory_order_relaxed);
        const int encodeStageResult = encodeResult.load(std::memory_order_relaxed);

        if (stopRequested_.load(std::memory_order_relaxed) ||
            demuxStageResult == AVERROR_EXIT ||
            decodeStageResult == AVERROR_EXIT ||
            trackRenderStageResult == AVERROR_EXIT ||
            encodeStageResult == AVERROR_EXIT) {
            logger_.Warn("收到停止请求，流水线退出。");
            exitCode_ = 130;
            return;
        }

        if (demuxStageResult < 0) {
            logger_.Error("Demux 阶段失败: " + ffmpeg::AvErrorToString(demuxStageResult));
            exitCode_ = 5;
            return;
        }

        if (decodeStageResult < 0) {
            logger_.Error("Decode 阶段失败: " + ffmpeg::AvErrorToString(decodeStageResult));
            exitCode_ = 6;
            return;
        }

        if (trackRenderStageResult < 0) {
            logger_.Error("TrackRender 阶段失败: " + ffmpeg::AvErrorToString(trackRenderStageResult));
            exitCode_ = 7;
            return;
        }

        if (encodeStageResult < 0) {
            logger_.Error("Encode 阶段失败: " + ffmpeg::AvErrorToString(encodeStageResult));
            exitCode_ = 8;
            return;
        }

        logger_.Info(
            "AVPacket->AVFrame->TrackRender->Encode flow ready, encoded frames: " +
            std::to_string(encodeStage.encoded_frame_count()) +
            ", detector calls: " +
            std::to_string(trackRenderStage.detector_invocations()) +
            ", max active tracks: " +
            std::to_string(trackRenderStage.max_active_track_count()));

        logger_.Info("四级流水线已打通，输出视频已由 FFmpeg 编码与封装阶段写出。");

        logger_.Info("Bootstrap pipeline finished.");
        exitCode_ = 0;
    }

    void LogConfiguration() {
        logger_.Info("========== HyperMosaic Config ==========");
        logger_.Info("input: " + PathToDisplayString(options_.demux.inputPath));
        logger_.Info("output: " + PathToDisplayString(options_.encode.outputPath));
        logger_.Info("target: " + std::string(ToString(options_.trackRender.targetType)));
        logger_.Info("decode workers: " + std::to_string(options_.decode.workerCount));
        logger_.Info("packet queue capacity: " + std::to_string(options_.demux.packetQueueCapacity));
        logger_.Info("frame queue capacity: " + std::to_string(options_.trackRender.frameQueueCapacity));
        logger_.Info("detector interval: " + std::to_string(options_.trackRender.detectorInterval));
        if (options_.trackRender.targetType == TargetType::Face) {
            logger_.Info(
                "face cascade: " +
                (options_.trackRender.faceCascadeModelPath.empty()
                     ? std::string("<auto-discovery>")
                     : PathToDisplayString(options_.trackRender.faceCascadeModelPath)));
        }
        logger_.Info(std::string("hw encode: ") + (options_.encode.enableHardware ? "enabled" : "disabled"));
        if (options_.encode.enableHardware) {
            logger_.Info("hw device: " + options_.encode.hwDevice);
        }
        logger_.Info(std::string("dry run: ") + (options_.dryRun ? "true" : "false"));
    }

    void LogDataFlow() {
        logger_.Info("========== Pipeline Topology ==========");
        logger_.Info("[Demux Thread] unique_ptr<AVPacket> -> MPSCQueue<Packet>");
        logger_.Info("[Decode Dispatcher] packet queue -> worker queues (round-robin)");
        logger_.Info("[Decode Worker Pool] each worker owns AVCodecContext -> shared_ptr<AVFrameWrapper>");
        logger_.Info("[Track & Render Thread] detector interval trigger + tracker predict/update + Y-plane mosaic");
        logger_.Info("[Encode Thread] consume rendered frames -> ffmpeg encode + mux output file");
    }

    PipelineOptions options_;
    ConsoleLogger& logger_;
    std::atomic_bool& stopRequested_;

    std::atomic_bool started_{false};
    std::thread worker_;
    int exitCode_{0};
};

class PipelineFactory {
public:
    static std::unique_ptr<IPipeline> Create(PipelineOptions options, ConsoleLogger& logger, std::atomic_bool& stopRequested) {
        return std::make_unique<BootstrapPipeline>(std::move(options), logger, stopRequested);
    }
};

class SignalBridge {
public:
    explicit SignalBridge(std::atomic_bool& stopRequested)
        : stopRequested_(stopRequested) {
        globalStopFlag_ = &stopRequested_;
        previousInt_ = std::signal(SIGINT, &SignalBridge::OnSignal);
#ifdef SIGTERM
        previousTerm_ = std::signal(SIGTERM, &SignalBridge::OnSignal);
#endif
#ifdef SIGBREAK
        previousBreak_ = std::signal(SIGBREAK, &SignalBridge::OnSignal);
#endif
    }

    SignalBridge(const SignalBridge&) = delete;
    SignalBridge& operator=(const SignalBridge&) = delete;

    ~SignalBridge() {
        std::signal(SIGINT, previousInt_);
#ifdef SIGTERM
        std::signal(SIGTERM, previousTerm_);
#endif
#ifdef SIGBREAK
        std::signal(SIGBREAK, previousBreak_);
#endif
        globalStopFlag_ = nullptr;
    }

private:
    static void OnSignal(int) {
        if (globalStopFlag_ != nullptr) {
            globalStopFlag_->store(true, std::memory_order_relaxed);
        }
    }

    using SignalHandler = void (*)(int);

    std::atomic_bool& stopRequested_;
    SignalHandler previousInt_{SIG_DFL};
    SignalHandler previousTerm_{SIG_DFL};
#ifdef SIGBREAK
    SignalHandler previousBreak_{SIG_DFL};
#endif

    static inline std::atomic_bool* globalStopFlag_{nullptr};
};

int RunApplication(int argc, const char* const argv[]) {
    const auto parse = CliParser::Parse(argc, argv);
    const std::string exeName = argc > 0 ? argv[0] : "hyper_mosaic";

    if (parse.status == ParseStatus::Help) {
        CliParser::PrintHelp(std::cout, exeName);
        return 0;
    }

    if (parse.status == ParseStatus::Error) {
        std::cerr << "参数错误: " << parse.message << "\n\n";
        CliParser::PrintHelp(std::cerr, exeName);
        return 1;
    }

    AppConfig config = parse.config;
    std::string validationError;
    if (!ValidateAndPrepareConfig(config, validationError)) {
        std::cerr << validationError << '\n';
        return 2;
    }

    std::atomic_bool stopRequested{false};
    SignalBridge signalBridge(stopRequested);

    ConsoleLogger logger;
    auto pipeline = PipelineFactory::Create(BuildPipelineOptions(config), logger, stopRequested);
    if (!pipeline) {
        std::cerr << "Pipeline 创建失败\n";
        return 3;
    }

    if (!pipeline->Start()) {
        std::cerr << "Pipeline 启动失败\n";
        return 4;
    }

    std::atomic_bool watcherStop{false};
    std::thread stopWatcher([&]() {
        while (!watcherStop.load(std::memory_order_relaxed)) {
            if (stopRequested.load(std::memory_order_relaxed)) {
                pipeline->RequestStop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    const int code = pipeline->Wait();
    watcherStop.store(true, std::memory_order_relaxed);
    if (stopWatcher.joinable()) {
        stopWatcher.join();
    }

    return code;
}

}  // namespace hm

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {

    // 强制分配一个控制台（如果当前没有）
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    std::cout.clear();
    std::cerr.clear();
    
    std::cout << "=== HyperMosaic started (console attached) ===" << std::endl;



    std::vector<std::string> utf8Args;
    utf8Args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        utf8Args.push_back(hm::WideToUtf8(argv[i]));
    }

    std::vector<const char*> rawArgs;
    rawArgs.reserve(utf8Args.size());
    for (const auto& arg : utf8Args) {
        rawArgs.push_back(arg.c_str());
    }

    return hm::RunApplication(static_cast<int>(rawArgs.size()), rawArgs.data());
}
#else
int main(int argc, char* argv[]) {
    std::vector<const char*> rawArgs;
    rawArgs.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        rawArgs.push_back(argv[i]);
    }

    return hm::RunApplication(argc, rawArgs.data());
}
#endif
