#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "iot/config.hpp"
#include "iot/domain.hpp"

namespace iot::services {

class InferenceBackend {
 public:
  virtual ~InferenceBackend() = default;
  virtual int infer(const float* channel_major_data, std::size_t length) = 0;
};

class InferenceEngine {
 public:
  explicit InferenceEngine(const InferenceConfig& config);
  InferenceEngine(const InferenceConfig& config, std::unique_ptr<InferenceBackend> backend);
  ~InferenceEngine();
  std::optional<std::string> process(const SensorSample& sample);
  bool ready() const noexcept { return static_cast<bool>(backend_); }  // 注意这个就是完整的定义

 private:
  struct DeviceWindow {
    std::deque<Vec3> samples;
    std::size_t full_window_updates{};
  };
  InferenceConfig config_;
  std::unique_ptr<InferenceBackend> backend_;
  std::unordered_map<std::string, DeviceWindow> windows_;
  std::mutex mutex_;
};
}  // namespace iot::services
