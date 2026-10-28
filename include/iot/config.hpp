#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iot {

struct EndpointConfig {
  std::string host{"0.0.0.0"};
  std::uint16_t port{};
};

struct SensorProfile {
  std::string name;
  int frequency_hz{25};
  std::uint16_t subscribe_tag{0x0fff};  // IMU948 fields; legacy default when omitted.
  std::uint16_t notify_handle{};
  std::uint16_t cccd_handle{};
  std::uint16_t write_handle{};
  std::optional<std::uint16_t> command_handle;
  std::optional<std::uint16_t> battery_handle;  // MD9930 FFF2 read value handle.
};

struct DeviceConfig {
  std::string mac;
  std::string type;
  std::string id;
  bool enabled{true};
};

struct InferenceConfig {
  bool enabled{true};
  std::filesystem::path engine_path{"model/convtran.engine"};
  std::size_t window_size{50};
  std::size_t stride{5};
};

struct QueueConfig {
  std::size_t database_capacity{30000};
  std::size_t inference_capacity{5000};
  std::size_t websocket_capacity{4096};
};

struct CollectionSchedule {
  bool enabled{false};
  std::string start_time{"14:00"};
  std::string stop_time{"17:00"};

  void validate() const;
  bool allows(std::chrono::system_clock::time_point now) const;
};

struct AlertConfig {
  bool enabled{false};
  int timeout_seconds{300};
  int retry_interval_seconds{60};
  int smtp_timeout_seconds{15};
  std::string smtp_host;
  int smtp_port{465};
  std::string smtp_security{"ssl"};  // ssl, starttls, none
  std::string smtp_username;
  std::string smtp_password;
  std::string from_email;
  std::string to_email;
};

struct AppConfig {
  EndpointConfig tcp{"0.0.0.0", 7628};
  EndpointConfig websocket{"0.0.0.0", 8765};
  std::string gateway_mac{"FF:FF:FF:FF:FF:FF"};
  int battery_interval_seconds{30};
  int reconnect_interval_seconds{60};
  int retention_days{7};
  std::filesystem::path data_dir{"data"};
  InferenceConfig inference;
  QueueConfig queues;
  CollectionSchedule collection_schedule;
  AlertConfig alerts;
  std::unordered_map<std::string, SensorProfile> sensor_types;
  std::vector<DeviceConfig> devices;  // 这里是devices

  static AppConfig load(const std::filesystem::path& path);
  const SensorProfile& profile_for_mac(const std::string& mac) const;
  bool is_allowed_device(const std::string& mac) const;
  void validate() const;
};

std::string normalize_mac(std::string mac);

}  // namespace iot
