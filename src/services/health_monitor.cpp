#include "iot/services/health_monitor.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

#include "iot/log.hpp"

namespace iot::services {
namespace {
std::string beijing(Clock::time_point time) {
  const auto shifted = Clock::to_time_t(time + std::chrono::hours(8));
  std::tm tm{};
  gmtime_r(&shifted, &tm);
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " 北京时间 (UTC+8)";
  return out.str();
}
}  // namespace

bool HealthTracker::reconcile(Clock::time_point wall, Steady::time_point monotonic) {
  const bool active = config_.alerts.enabled && config_.collection_schedule.allows(wall);
  if (!active) {
    states_.clear();
    monitoring_ = false;
    return false;
  }
  if (!monitoring_) {
    monitoring_ = true;
    for (const auto& device : config_.devices)
      states_[device.mac] = State{wall, monotonic, monotonic, false, false, ++generation_};
  }
  return true;
}

void HealthTracker::sample(const std::string& mac, Clock::time_point wall, Steady::time_point monotonic) {
  if (!reconcile(wall, monotonic)) return;
  const auto it = states_.find(normalize_mac(mac));
  if (it == states_.end()) return;
  it->second = State{wall, monotonic, monotonic, true, false, ++generation_};
}

std::vector<DeviceAlert> HealthTracker::due(Clock::time_point wall, Steady::time_point monotonic) {
  std::vector<DeviceAlert> result;
  if (!reconcile(wall, monotonic)) return result;
  for (const auto& device : config_.devices) {
    auto& state = states_.at(device.mac);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(monotonic - state.last).count();
    if (state.reported || monotonic < state.retry || elapsed < config_.alerts.timeout_seconds) continue;
    result.push_back({device.id, device.mac, state.since, wall, elapsed, state.had_sample, state.generation});
    state.retry = monotonic + std::chrono::seconds(config_.alerts.retry_interval_seconds);
  }
  return result;
}

void HealthTracker::delivered(const std::vector<DeviceAlert>& alerts) {
  for (const auto& alert : alerts) {
    const auto it = states_.find(alert.mac);
    // A sample received while SMTP was sending belongs to a new incident.
    if (it != states_.end() && it->second.generation == alert.generation) it->second.reported = true;
  }
}

std::string alert_body(const std::vector<DeviceAlert>& alerts, int timeout_seconds) {
  std::ostringstream out;
  out << "传感器采集超时报警\n判定：应采集时连续 " << timeout_seconds
      << " 秒未收到有效采样数据。\n\n";
  for (const auto& alert : alerts) {
    out << "设备 ID: " << alert.id << "\nMAC: " << alert.mac
        << "\n无数据起始时间: " << beijing(alert.missing_since)
        << (alert.had_sample ? "（最后一次有效采样）" : "（开始监测后尚未收到采样）")
        << "\n报警时间: " << beijing(alert.detected_at)
        << "\n持续无数据: " << alert.missing_seconds << " 秒\n\n";
  }
  out << "请检查传感器、电量、网关与网络。无数据不代表已定位具体硬件故障。\n"
      << "同一次故障成功通知后不重复发送；恢复采样后再次超时会重新通知。\n";
  return out.str();
}

HealthMonitor::HealthMonitor(const AppConfig& config) : config_(config), tracker_(config) {}
HealthMonitor::~HealthMonitor() { stop(); }

void HealthMonitor::start() {
  if (!config_.alerts.enabled || worker_.joinable()) return;
  stopping_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tracker_.due(Clock::now(), HealthTracker::Steady::now());
  }
  worker_ = std::thread(&HealthMonitor::run, this);
  log(LogLevel::info, "device email alerts enabled; timeout=" + std::to_string(config_.alerts.timeout_seconds) + "s");
}

void HealthMonitor::stop() {
  stopping_ = true;
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void HealthMonitor::sample(const std::string& mac) {
  if (!config_.alerts.enabled || stopping_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  tracker_.sample(mac, Clock::now(), HealthTracker::Steady::now());
}

void HealthMonitor::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    auto alerts = tracker_.due(Clock::now(), HealthTracker::Steady::now());
    if (!alerts.empty()) {
      lock.unlock();
      for (const auto& alert : alerts)
        log(LogLevel::warning, "sensor timeout: id=" + alert.id + " mac=" + alert.mac +
            " missing_since=" + beijing(alert.missing_since));
      const bool success = send_alert_email(config_.alerts, alerts, stopping_);
      log(success ? LogLevel::info : LogLevel::warning,
          success ? "sensor alert accepted by SMTP server" : "sensor alert delivery failed or cancelled; will retry");
      lock.lock();
      if (success) tracker_.delivered(alerts);
    }
    wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_.load(); });
  }
}
}  // namespace iot::services
