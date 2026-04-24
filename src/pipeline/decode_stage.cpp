#include "decode_stage.h"

#include <cerrno>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace hm::pipeline {
namespace {

void SetFirstError(std::atomic_int& firstError, int errorCode) {
    if (errorCode >= 0) {
        return;
    }

    int expected = 0;
    firstError.compare_exchange_strong(expected, errorCode, std::memory_order_relaxed);
}

}  // namespace

DecodeThreadPool::DecodeThreadPool(
    DecodeStageOptions options,
    PacketQueue& inputQueue,
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
}

int DecodeThreadPool::ValidateOptions() const {
    if (options_.workerCount == 0 || options_.workerCount > 256) {
        return AVERROR(EINVAL);
    }

    if (options_.workerQueueCapacity == 0) {
        return AVERROR(EINVAL);
    }

    if (options_.videoStreamIndex < 0) {
        return AVERROR(EINVAL);
    }

    if (options_.codecParameters == nullptr) {
        return AVERROR(EINVAL);
    }

    if (options_.codecParameters->codec_id == AV_CODEC_ID_NONE) {
        return AVERROR_DECODER_NOT_FOUND;
    }

    return 0;
}

int DecodeThreadPool::Run() {
    const int validateResult = ValidateOptions();
    if (validateResult < 0) {
        LogError("Decode options invalid: " + ffmpeg::AvErrorToString(validateResult));
        outputQueue_.Close();
        return validateResult;
    }

    const AVCodec* decoder = avcodec_find_decoder(options_.codecParameters->codec_id);
    if (decoder == nullptr) {
        LogError("Decoder not found for codec_id=" + std::to_string(options_.codecParameters->codec_id));
        outputQueue_.Close();
        return AVERROR_DECODER_NOT_FOUND;
    }

    std::size_t effectiveWorkerCount = options_.workerCount;
    if (effectiveWorkerCount > 1) {
        LogInfo("当前版本为保证跨帧参考正确性，decode worker 强制回退为 1（多上下文切分将在后续版本实现）。");
        effectiveWorkerCount = 1;
    }

    std::vector<std::unique_ptr<PacketQueue>> workerQueues;
    workerQueues.reserve(effectiveWorkerCount);
    for (std::size_t i = 0; i < effectiveWorkerCount; ++i) {
        workerQueues.push_back(std::make_unique<PacketQueue>(options_.workerQueueCapacity));
    }

    std::atomic_int firstError{0};
    std::atomic_ullong totalFrameCount{0};
    std::atomic_ullong decodeOrderCounter{0};

    auto decodeFrames = [&](AVCodecContext* codecContext, int64_t packetPts, int workerId) -> int {
        for (;;) {
            ffmpeg::FramePtr frame = ffmpeg::MakeFrame();
            if (!frame) {
                return AVERROR(ENOMEM);
            }

            const int receiveResult = avcodec_receive_frame(codecContext, frame.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return 0;
            }
            if (receiveResult < 0) {
                return receiveResult;
            }

            FrameWrapperPtr frameWrapper = std::make_shared<AVFrameWrapper>();
            if (!frameWrapper) {
                return AVERROR(ENOMEM);
            }

            frameWrapper->bestEffortPts = frame->best_effort_timestamp;
            frameWrapper->packetPts = packetPts;
            frameWrapper->decodeOrder = decodeOrderCounter.fetch_add(1, std::memory_order_relaxed);
            frameWrapper->workerId = workerId;
            frameWrapper->sourceTimeBase = options_.videoTimeBase;
            frameWrapper->frame = std::move(frame);

            if (!outputQueue_.Push(std::move(frameWrapper), &stopRequested_)) {
                return stopRequested_.load(std::memory_order_relaxed) ? AVERROR_EXIT : AVERROR(EIO);
            }

            totalFrameCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto workerMain = [&](std::size_t workerId) {
        ffmpeg::CodecContextPtr codecContext = ffmpeg::MakeCodecContext(decoder);
        if (!codecContext) {
            SetFirstError(firstError, AVERROR(ENOMEM));
            inputQueue_.Close();
            return;
        }

        int result = avcodec_parameters_to_context(codecContext.get(), options_.codecParameters);
        if (result < 0) {
            SetFirstError(firstError, result);
            inputQueue_.Close();
            return;
        }

        codecContext->pkt_timebase = options_.videoTimeBase;
        result = avcodec_open2(codecContext.get(), decoder, nullptr);
        if (result < 0) {
            SetFirstError(firstError, result);
            inputQueue_.Close();
            return;
        }

        PacketPtr packet;
        while (workerQueues[workerId]->Pop(packet, &stopRequested_)) {
            if (stopRequested_.load(std::memory_order_relaxed)) {
                break;
            }

            const int64_t packetPts = packet ? packet->pts : AV_NOPTS_VALUE;

            result = avcodec_send_packet(codecContext.get(), packet.get());
            if (result == AVERROR(EAGAIN)) {
                result = decodeFrames(codecContext.get(), packetPts, static_cast<int>(workerId));
                if (result < 0) {
                    SetFirstError(firstError, result);
                    inputQueue_.Close();
                    return;
                }
                result = avcodec_send_packet(codecContext.get(), packet.get());
            }

            if (result < 0) {
                SetFirstError(firstError, result);
                inputQueue_.Close();
                return;
            }

            result = decodeFrames(codecContext.get(), packetPts, static_cast<int>(workerId));
            if (result < 0) {
                SetFirstError(firstError, result);
                inputQueue_.Close();
                return;
            }
        }

        if (firstError.load(std::memory_order_relaxed) < 0) {
            return;
        }

        result = avcodec_send_packet(codecContext.get(), nullptr);
        if (result == AVERROR_EOF) {
            return;
        }
        if (result < 0) {
            SetFirstError(firstError, result);
            return;
        }

        result = decodeFrames(codecContext.get(), AV_NOPTS_VALUE, static_cast<int>(workerId));
        if (result < 0) {
            SetFirstError(firstError, result);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(effectiveWorkerCount);
    for (std::size_t i = 0; i < effectiveWorkerCount; ++i) {
        workers.emplace_back(workerMain, i);
    }

    std::size_t nextWorker = 0;
    std::uint64_t dispatchedPackets = 0;
    PacketPtr packet;
    while (!stopRequested_.load(std::memory_order_relaxed) && firstError.load(std::memory_order_relaxed) == 0) {
        if (!inputQueue_.Pop(packet, &stopRequested_)) {
            break;
        }

        if (!packet || packet->stream_index != options_.videoStreamIndex) {
            continue;
        }

        PacketQueue& targetQueue = *workerQueues[nextWorker];
        nextWorker = (nextWorker + 1) % workerQueues.size();

        if (!targetQueue.Push(std::move(packet), &stopRequested_)) {
            if (!stopRequested_.load(std::memory_order_relaxed)) {
                SetFirstError(firstError, AVERROR(EIO));
            }
            break;
        }

        ++dispatchedPackets;
    }

    for (auto& workerQueue : workerQueues) {
        workerQueue->Close();
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    outputQueue_.Close();

    const int errorCode = firstError.load(std::memory_order_relaxed);
    if (errorCode < 0) {
        LogError("Decode stage failed: " + ffmpeg::AvErrorToString(errorCode));
        return errorCode;
    }

    if (stopRequested_.load(std::memory_order_relaxed)) {
        LogInfo("Decode stop requested.");
        return AVERROR_EXIT;
    }

    std::ostringstream oss;
    oss << "Decode completed, workers=" << effectiveWorkerCount
        << ", packets_dispatched=" << dispatchedPackets
        << ", frames_output=" << totalFrameCount.load(std::memory_order_relaxed);
    LogInfo(oss.str());

    return 0;
}

void DecodeThreadPool::LogInfo(std::string_view message) const {
    if (infoLog_) {
        infoLog_(message);
    }
}

void DecodeThreadPool::LogError(std::string_view message) const {
    if (errorLog_) {
        errorLog_(message);
    }
}

}  // namespace hm::pipeline
