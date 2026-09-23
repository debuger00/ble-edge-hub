#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace iot::concurrency {

enum class OverflowPolicy {
  reject_newest,
  drop_oldest,
};

enum class PushResult {
  pushed,
  dropped_oldest,
  rejected,
  closed,
};

// A bounded, multi-producer/single-or-multi-consumer FIFO queue.
// close() rejects new messages, wakes blocked consumers, and lets them drain
// messages that were already accepted before wait_pop() returns std::nullopt.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity, OverflowPolicy overflow_policy = OverflowPolicy::reject_newest)
      : capacity_(capacity), overflow_policy_(overflow_policy) {
    if (capacity_ == 0) throw std::invalid_argument("queue capacity must be positive");
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  PushResult try_push(T value) {
    PushResult result = PushResult::pushed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return PushResult::closed;
      if (queue_.size() >= capacity_) {
        ++dropped_;
        if (overflow_policy_ == OverflowPolicy::reject_newest) return PushResult::rejected;
        queue_.pop_front();
        result = PushResult::dropped_oldest;
      }
      queue_.push_back(std::move(value));
    }
    ready_.notify_one();
    return result;
  }

  std::optional<T> wait_pop() {  //等待并取出数据
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return std::nullopt;
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return;
      closed_ = true;
    }
    ready_.notify_all();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  std::size_t dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

  std::size_t capacity() const noexcept { return capacity_; }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  const std::size_t capacity_;
  const OverflowPolicy overflow_policy_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<T> queue_;
  std::size_t dropped_{};
  bool closed_{false};
};

}  // namespace iot::concurrency
