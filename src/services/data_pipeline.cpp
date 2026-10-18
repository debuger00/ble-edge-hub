#include "iot/services/data_pipeline.hpp"

#include <string>
#include <utility>

#include "iot/json.hpp"
#include "iot/log.hpp"
#include "iot/services/health_monitor.hpp"
#include "iot/services/database_writer.hpp"
#include "iot/services/inference_worker.hpp"
#include "iot/services/websocket_server.hpp"

namespace iot::services {
//分发器，消息队列 由各自 消费者线程实现
DataPipeline::DataPipeline(DatabaseWriter& database, InferenceWorker& inference, WebSocketServer& websocket,
                           HealthMonitor* health)
    : health_(health), database_(database), inference_(inference), websocket_(websocket) {}  // 这里三个消费者线程

void DataPipeline::publish(SensorSample sample) {
  if (health_) health_->sample(sample.mac);
  // 这里publish
  if (!database_.enqueue(sample)) {
    const auto count = database_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
    report_drop("database", count);
  }

  const auto json = sample_json(sample);
  if (!websocket_.broadcast(json, sample.mac, true, epoch_millis(sample.received_at))) {
    const auto count = websocket_rejected_.fetch_add(1, std::memory_order_relaxed) + 1;
    report_drop("websocket", count);
  }

  if (!inference_.enqueue(std::move(sample))) {  //这里使用move 比较好啊， 是move 的使用场景
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
  //当某条处理路径拒绝消息时，按累计次数打印警告，避免每次失败都打印而刷屏。
  if (count == 1 || count % 1000 == 0) {  // 只在第 1 次、以及每满 1000 次时打日志
    log(LogLevel::warning, std::string(channel) + " pipeline rejected messages=" + std::to_string(count));
  }
}

}  // namespace iot::services
