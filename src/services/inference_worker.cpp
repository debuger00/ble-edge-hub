#include "iot/services/inference_worker.hpp"

#include <exception>
#include <utility>

#include "iot/log.hpp"

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
  worker_ = std::thread(&InferenceWorker::run, this);  // 这里调用推理
  log(LogLevel::info, "inference worker started; queue capacity=" + std::to_string(queue_.capacity()));
}

// 这是Pipeline 推理送进来的第一个 把一条采样投递进推理队列，返回「是否被接受」（true = 收下，false = 拒绝）。
bool InferenceWorker::enqueue(SensorSample sample) {
  if (!engine_.ready()) return true;  // 引擎未就绪：直接丢弃但装作成功，避免误报丢弃告警
  if (!started_) return false;        // 推理线程没启动：队列无人消费，明确拒绝
  const auto result = queue_.try_push(std::move(sample));  // 移动进有界队列
  if (result == concurrency::PushResult::dropped_oldest) report_drop(queue_.dropped());  // 满了丢最旧，记日志
  // 新样本被收下（含「丢旧换新」）就算成功，只有拒绝/已关闭才算失败。
  return result == concurrency::PushResult::pushed || result == concurrency::PushResult::dropped_oldest;
}

void InferenceWorker::stop() {
  if (!started_.exchange(false)) return;
  queue_.close();
  if (worker_.joinable()) worker_.join();
  log(LogLevel::info, "inference worker stopped; dropped=" + std::to_string(queue_.dropped()));
}

// 推理线程主循环：阻塞式取数据、跑推理、回调结果，出错只记日志不中断。
void InferenceWorker::run() {
  // 队列有数据就取出（否则阻塞等待）；队列 close 且排空后 wait_pop 返回 nullopt，循环退出。
  while (auto sample = queue_.wait_pop()) {
    try {
      // 交给推理引擎处理，返回可选的动作标签（无预测则返回 nullopt）。
      if (const auto action = engine_.process(*sample)) {
        if (prediction_handler_) prediction_handler_(sample->mac, *action);  // 回调预测结果
      }
    } catch (const std::exception& error) {
      log(LogLevel::error, std::string("inference task failed: ") + error.what());  // 单条失败不影响线程
    }
  }
}

void InferenceWorker::report_drop(std::size_t dropped) {
  if (dropped == 1 || dropped % 1000 == 0) {
    log(LogLevel::warning, "inference queue overloaded; dropped oldest samples=" + std::to_string(dropped));
  }
}

}  // namespace iot::services
