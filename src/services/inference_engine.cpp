#include "iot/services/inference_engine.hpp"

#include "iot/log.hpp"
#ifdef IOT_ENABLE_TENSORRT
#include "iot/inference/tensorrt_backend.hpp"
#endif

#include <array>
#include <vector>

namespace iot::services {
namespace {
constexpr std::array<const char*, 5> kClasses{"standing", "running", "grazing", "trotting", "walking"};
}

InferenceEngine::InferenceEngine(const InferenceConfig& config) : config_(config) {
#ifdef IOT_ENABLE_TENSORRT
  if (config_.enabled) {
    try {
      backend_ = inference::make_tensorrt_backend(config_.engine_path, config_.window_size * 3, kClasses.size());
      log(LogLevel::info, "TensorRT engine loaded: " + config_.engine_path.string());
    } catch (const std::exception& error) {
      log(LogLevel::error, std::string("TensorRT disabled: ") + error.what());
    }
  }
#else
  if (config_.enabled) log(LogLevel::warning, "binary was built without TensorRT; inference is disabled");
#endif
}

InferenceEngine::InferenceEngine(const InferenceConfig& config, std::unique_ptr<InferenceBackend> backend)
    : config_(config), backend_(std::move(backend)) {}
InferenceEngine::~InferenceEngine() = default;

std::optional<std::string> InferenceEngine::process(const SensorSample& sample) {
  if (!backend_ || !sample.accel) return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& window = windows_[sample.mac];
  window.samples.push_back(*sample.accel);
  if (window.samples.size() > config_.window_size) window.samples.pop_front();
  if (window.samples.size() != config_.window_size) return std::nullopt;
  if (++window.full_window_updates < config_.stride) return std::nullopt;
  window.full_window_updates = 0;

  std::vector<float> input(config_.window_size * 3);
  for (std::size_t time = 0; time < window.samples.size(); ++time) {
    input[time] = static_cast<float>(window.samples[time].x);
    input[config_.window_size + time] = static_cast<float>(window.samples[time].y);
    input[2 * config_.window_size + time] = static_cast<float>(window.samples[time].z);
  }
  const int index = backend_->infer(input.data(), input.size());
  if (index < 0 || static_cast<std::size_t>(index) >= kClasses.size()) {
    log(LogLevel::error, "TensorRT returned an invalid class index: " + std::to_string(index));
    return std::nullopt;
  }
  return std::string(kClasses[static_cast<std::size_t>(index)]);
}

}  // namespace iot::services

