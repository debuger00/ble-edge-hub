#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "iot/config.hpp"
#include "iot/domain.hpp"

namespace iot::services {

struct DeviceAlert {
  std::string id;
  std::string mac;
  Clock::time_point missing_since;
  Clock::time_point detected_at;
  std::int64_t missing_seconds{};
  bool had_sample{};
  std::uint64_t generation{};
};

// Deterministic state machine; its owner provides synchronization and both clocks.
class HealthTracker {
 public:
  using Steady = std::chrono::steady_clock;
  explicit HealthTracker(const AppConfig& config) : config_(config) {}
  void sample(const std::string& mac, Clock::time_point wall, Steady::time_point monotonic);
  std::vector<DeviceAlert> due(Clock::time_point wall, Steady::time_point monotonic);
  void delivered(const std::vector<DeviceAlert>& alerts);

 private:
  struct State {
    Clock::time_point since;
    Steady::time_point last;
    Steady::time_point retry;
    bool had_sample{false};
    bool reported{false};
    std::uint64_t generation{};
  };
  bool reconcile(Clock::time_point wall, Steady::time_point monotonic);
  const AppConfig& config_;
  bool monitoring_{false};
  std::uint64_t generation_{};
  std::unordered_map<std::string, State> states_;
};

std::string alert_body(const std::vector<DeviceAlert>& alerts, int timeout_seconds);
// SMTP runs in a child process with credentials on stdin, never shell arguments.
bool send_alert_email(const AlertConfig& config, const std::vector<DeviceAlert>& alerts,
                      const std::atomic<bool>& stopping);

class HealthMonitor {
 public:
  explicit HealthMonitor(const AppConfig& config);
  ~HealthMonitor();
  void start();
  void stop();
  void sample(const std::string& mac);

 private:
  void run();
  const AppConfig& config_;
  HealthTracker tracker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
};

}  // namespace iot::services
