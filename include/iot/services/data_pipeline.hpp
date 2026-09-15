#pragma once

#include <atomic>
#include <cstddef>

#include "iot/domain.hpp"

namespace iot::services {

class DatabaseWriter;
class InferenceWorker;
class WebSocketServer;

// Fans each parsed sensor event out to independent downstream queues. All
// methods are intentionally non-blocking so a slow disk, GPU, or browser does
// not stop the TCP receive loop.
class DataPipeline {
 public:
  DataPipeline(DatabaseWriter& database, InferenceWorker& inference, WebSocketServer& websocket);

  void publish(SensorSample sample);
  void publish(const SensorStatus& status);

  std::size_t database_dropped() const noexcept { return database_dropped_; }
  std::size_t inference_rejected() const noexcept { return inference_rejected_; }
  std::size_t websocket_rejected() const noexcept { return websocket_rejected_; }

 private:
  static void report_drop(const char* channel, std::size_t count);

  DatabaseWriter& database_;
  InferenceWorker& inference_;
  WebSocketServer& websocket_;
  std::atomic<std::size_t> database_dropped_{0};  // 这里使用原子类型的原因，是防止多线程是写入过程 中断
  std::atomic<std::size_t> inference_rejected_{0};
  std::atomic<std::size_t> websocket_rejected_{0};
};

}  // namespace iot::services
