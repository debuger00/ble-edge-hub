#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iot {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::system_clock;

struct Vec3 {
  double x{};
  double y{};
  double z{};
};

struct SensorSample {
  std::string mac;
  std::string device_type;
  std::optional<Vec3> accel;
  std::optional<Vec3> raw_accel;
  std::optional<Vec3> gyro;
  std::optional<Vec3> mag;
  std::optional<std::uint64_t> sensor_uptime_ms;
  std::optional<std::uint16_t> counter;
  std::optional<std::uint32_t> ticks;           //设备的 tick 计数
  Clock::time_point received_at{Clock::now()};  //接收时间，默认初始化为创建对象时的当前时间
};

struct SensorStatus {
  std::string mac;
  std::uint8_t battery_level{};
  std::uint16_t battery_voltage_mv{};
  std::string firmware_version;
  std::string product_model;
};

enum class ConnectionState : std::uint8_t {
  disconnected = 0,
  disconnecting = 1,
  searching = 2,
  connecting = 3,
  connected = 4,
  enabling_notifications = 5,
  ready = 6,
  unknown = 0xff,
};

struct DeviceDataEvent {
  std::string mac;
  std::uint16_t handle{};
  Bytes data;
};

struct ConnectionEvent {
  std::string mac;
  ConnectionState state{ConnectionState::unknown};
};

struct ScanEvent {
  std::string mac;
  std::int8_t rssi{};
};

}  // namespace iot
