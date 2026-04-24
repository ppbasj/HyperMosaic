#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace hm::concurrent {

template <typename T>
class MPSCQueue {
public:
    explicit MPSCQueue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {
    }

    bool Push(T item, const std::atomic_bool* stopRequested = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        producerCv_.wait(lock, [&]() {
            return closed_ || queue_.size() < capacity_ ||
                   (stopRequested != nullptr && stopRequested->load(std::memory_order_relaxed));
        });

        if (closed_ || (stopRequested != nullptr && stopRequested->load(std::memory_order_relaxed))) {
            return false;
        }

        queue_.push(std::move(item));
        consumerCv_.notify_one();
        return true;
    }

    bool Pop(T& out, const std::atomic_bool* stopRequested = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        consumerCv_.wait(lock, [&]() {
            return closed_ || !queue_.empty() ||
                   (stopRequested != nullptr && stopRequested->load(std::memory_order_relaxed));
        });

        if (queue_.empty()) {
            return false;
        }

        out = std::move(queue_.front());
        queue_.pop();
        producerCv_.notify_one();
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        producerCv_.notify_all();
        consumerCv_.notify_all();
    }

    [[nodiscard]] bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t Capacity() const noexcept {
        return capacity_;
    }

private:
    const std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable producerCv_;
    std::condition_variable consumerCv_;
    std::queue<T> queue_;
    bool closed_{false};
};

}  // namespace hm::concurrent
