#include "iot/services/data_pipeline.hpp"

#include "iot/json.hpp"
#include "iot/log.hpp"
#include "iot/services/database_writer.hpp"
#include "iot/services/inference_worker.hpp"
#include "iot/services/websocket_server.hpp"

#include <string>
#include <utility>

namespace iot::services {

DataPipeline::DataPipeline(DatabaseWriter& database, InferenceWorker& inference,
                           WebSocketServer& websocket)
    : database_(database), inference_(inference), websocket_(websocket) {}

void DataPipeline::publish(SensorSample sample) {
  if (!database_.enqueue(sample)) {
    const auto count = database_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
    report_drop("database", count);
  }

  const auto json = sample_json(sample);
  if (!websocket_.broadcast(json)) {
    const auto count = websocket_rejected_.fetch_add(1, std::memory_order_relaxed) + 1;
    report_drop("websocket", count);
  }

  if (!inference_.enqueue(std::move(sample))) {
    const auto count = inference_rejected_.fetch_add(1, std::memory_order_relaxed) + 1;
    report_drop("inference", count);
  }
}

void DataPipeline::publish(const SensorStatus& status) {
  if (!websocket_.broadcast(battery_json(status))) {
    const auto count = websocket_rejected_.fetch_add(1, std::memory_order_relaxed) + 1;
    report_drop("websocket", count);
  }
}

void DataPipeline::report_drop(const char* channel, std::size_t count) {
  if (count == 1 || count % 1000 == 0) {
    log(LogLevel::warning, std::string(channel) +
                               " pipeline rejected messages=" + std::to_string(count));
  }
}

}  // namespace iot::services
