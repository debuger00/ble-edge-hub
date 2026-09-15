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

// 把一个传感器采样数据序列化成 JSON 字符串，供 WebSocket 推送给浏览器。
inline std::string sample_json(const SensorSample& sample) {
  std::ostringstream out;
  // 头部：类型、mac、时间戳、设备上电时长；setprecision(10) 控制浮点数精度。
  out << std::setprecision(10) << "{\"type\":\"sensor_data\",\"mac\":\"" << json_escape(sample.mac)
      << "\",\"timestamp\":" << epoch_millis(sample.received_at) << ",\"sensor_uptime\":"
      << sample.sensor_uptime_ms.value_or(0) << ",\"data\":{";
  // first 用于标记是否已输出过字段，从而正确处理字段之间的逗号。
  bool first = true;
  append_vec(out, "accel", sample.accel, first);        // 加速度（m/s²）
  append_vec(out, "raw_accel", sample.raw_accel, first); // 原始加速度计读数
  append_vec(out, "gyro", sample.gyro, first);           // 陀螺仪（角速度）
  append_vec(out, "mag", sample.mag, first);             // 磁力计
  if (!first) out << ',';                               // 前面有向量字段才补逗号
  out << "\"device_type\":\"" << json_escape(sample.device_type) << "\"}}";  // 收尾：设备型号 + 两个右括号
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

