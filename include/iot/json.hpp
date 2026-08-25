#pragma once

#include "iot/domain.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

namespace iot {

inline std::string json_escape(const std::string& input) {
  std::ostringstream out;
  for (const unsigned char c : input) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        else out << c;
    }
  }
  return out.str();
}

inline std::int64_t epoch_millis(Clock::time_point time = Clock::now()) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

inline void append_vec(std::ostringstream& out, const char* name, const std::optional<Vec3>& value, bool& first) {
  if (!value) return;
  if (!first) out << ',';
  first = false;
  out << '"' << name << "\":{" << "\"x\":" << value->x << ",\"y\":" << value->y << ",\"z\":" << value->z << '}';
}

inline std::string sample_json(const SensorSample& sample) {
  std::ostringstream out;
  out << std::setprecision(10) << "{\"type\":\"sensor_data\",\"mac\":\"" << json_escape(sample.mac)
      << "\",\"timestamp\":" << epoch_millis(sample.received_at) << ",\"sensor_uptime\":"
      << sample.sensor_uptime_ms.value_or(0) << ",\"data\":{";
  bool first = true;
  append_vec(out, "accel", sample.accel, first);
  append_vec(out, "raw_accel", sample.raw_accel, first);
  append_vec(out, "gyro", sample.gyro, first);
  append_vec(out, "mag", sample.mag, first);
  if (!first) out << ',';
  out << "\"device_type\":\"" << json_escape(sample.device_type) << "\"}}";
  return out.str();
}

inline std::string battery_json(const SensorStatus& status) {
  return "{\"type\":\"battery\",\"mac\":\"" + json_escape(status.mac) +
         "\",\"timestamp\":" + std::to_string(epoch_millis()) +
         ",\"level\":" + std::to_string(status.battery_level) + "}";
}

inline std::string prediction_json(const std::string& mac, const std::string& action) {
  return "{\"type\":\"prediction\",\"mac\":\"" + json_escape(mac) +
         "\",\"timestamp\":" + std::to_string(epoch_millis()) +
         ",\"action\":\"" + json_escape(action) + "\"}";
}

}  // namespace iot

