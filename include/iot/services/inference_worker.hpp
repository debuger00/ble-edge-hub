#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "iot/concurrency/bounded_queue.hpp"
#include "iot/services/inference_engine.hpp"

namespace iot::services {

class InferenceWorker {
 public:
  using PredictionHandler = std::function<void(const std::string&, const std::string&)>;

  InferenceWorker(const InferenceConfig& config, PredictionHandler prediction_handler,
                  std::size_t queue_capacity);
  InferenceWorker(const InferenceConfig& config, std::unique_ptr<InferenceBackend> backend,
                  PredictionHandler prediction_handler, std::size_t queue_capacity);
  ~InferenceWorker();
  InferenceWorker(const InferenceWorker&) = delete;
  InferenceWorker& operator=(const InferenceWorker&) = delete;

  void start();
  bool enqueue(SensorSample sample);
  void stop();

  bool ready() const noexcept { return engine_.ready(); }
  std::size_t queued() const { return queue_.size(); }
  std::size_t dropped() const { return queue_.dropped(); }
  std::size_t capacity() const noexcept { return queue_.capacity(); }

 private:
  void run();
  void report_drop(std::size_t dropped);

  InferenceEngine engine_;
  PredictionHandler prediction_handler_;
  concurrency::BoundedQueue<SensorSample> queue_;  // 这里的 每个成员的， 都是一个采样点
  std::atomic<bool> started_{false};
  std::thread worker_;
};

}  // namespace iot::services
