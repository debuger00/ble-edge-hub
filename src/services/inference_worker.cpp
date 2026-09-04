#include "iot/services/inference_worker.hpp"

#include "iot/log.hpp"

#include <exception>
#include <utility>

namespace iot::services {

InferenceWorker::InferenceWorker(const InferenceConfig& config, PredictionHandler prediction_handler,
                                 std::size_t queue_capacity)
    : engine_(config),
      prediction_handler_(std::move(prediction_handler)),
      queue_(queue_capacity, concurrency::OverflowPolicy::drop_oldest) {}

InferenceWorker::InferenceWorker(const InferenceConfig& config, std::unique_ptr<InferenceBackend> backend,
                                 PredictionHandler prediction_handler, std::size_t queue_capacity)
    : engine_(config, std::move(backend)),
      prediction_handler_(std::move(prediction_handler)),
      queue_(queue_capacity, concurrency::OverflowPolicy::drop_oldest) {}

InferenceWorker::~InferenceWorker() { stop(); }

void InferenceWorker::start() {
  if (!engine_.ready() || started_.exchange(true)) return;
  worker_ = std::thread(&InferenceWorker::run, this);
  log(LogLevel::info, "inference worker started; queue capacity=" +
                          std::to_string(queue_.capacity()));
}

bool InferenceWorker::enqueue(SensorSample sample) {
  if (!engine_.ready()) return true;
  if (!started_) return false;
  const auto result = queue_.try_push(std::move(sample));
  if (result == concurrency::PushResult::dropped_oldest) report_drop(queue_.dropped());
  return result == concurrency::PushResult::pushed ||
         result == concurrency::PushResult::dropped_oldest;
}

void InferenceWorker::stop() {
  if (!started_.exchange(false)) return;
  queue_.close();
  if (worker_.joinable()) worker_.join();
  log(LogLevel::info, "inference worker stopped; dropped=" + std::to_string(queue_.dropped()));
}

void InferenceWorker::run() {
  while (auto sample = queue_.wait_pop()) {
    try {
      if (const auto action = engine_.process(*sample)) {
        if (prediction_handler_) prediction_handler_(sample->mac, *action);
      }
    } catch (const std::exception& error) {
      log(LogLevel::error, std::string("inference task failed: ") + error.what());
    }
  }
}

void InferenceWorker::report_drop(std::size_t dropped) {
  if (dropped == 1 || dropped % 1000 == 0) {
    log(LogLevel::warning, "inference queue overloaded; dropped oldest samples=" +
                               std::to_string(dropped));
  }
}

}  // namespace iot::services
