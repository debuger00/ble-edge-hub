#include "iot/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace iot {
namespace {

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::pair<std::string, std::string> key_value(const std::string& line) {
  const auto pos = line.find(':');
  if (pos == std::string::npos) return {trim(line), {}};
  return {trim(line.substr(0, pos)), trim(line.substr(pos + 1))};
}

int parse_int(const std::string& text) {
  std::size_t used{};
  const int result = std::stoi(text, &used, 0);
  if (used != text.size()) throw std::runtime_error("invalid integer: " + text);
  return result;
}

bool parse_bool(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  if (text == "true" || text == "yes" || text == "1") return true;
  if (text == "false" || text == "no" || text == "0") return false;
  throw std::runtime_error("invalid boolean: " + text);
}

}  // namespace

std::string normalize_mac(std::string mac) {
  std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char c) { return std::toupper(c); });
  return mac;
}

AppConfig AppConfig::load(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open config: " + path.string());

  AppConfig cfg;
  std::string section;
  std::string sensor_type;
  DeviceConfig* device = nullptr;
  std::string raw;
  std::size_t line_number = 0;
  try {
    while (std::getline(input, raw)) {
      ++line_number;
      const auto comment = raw.find('#');
      if (comment != std::string::npos) raw.erase(comment);
      const auto content = trim(raw);
      if (content.empty()) continue;
      const auto indent = raw.find_first_not_of(' ');
      auto [key, value] = key_value(content);

      if (indent == 0) {
        device = nullptr;
        sensor_type.clear();
        if (value.empty()) {
          section = key;
          continue;
        }
        if (key == "gateway_mac") cfg.gateway_mac = normalize_mac(value);
        else if (key == "battery_interval_seconds") cfg.battery_interval_seconds = parse_int(value);
        else if (key == "reconnect_interval_seconds") cfg.reconnect_interval_seconds = parse_int(value);
        else if (key == "retention_days") cfg.retention_days = parse_int(value);
        else if (key == "data_dir") cfg.data_dir = value;
        continue;
      }

      if (section == "tcp" || section == "websocket") {
        auto& endpoint = section == "tcp" ? cfg.tcp : cfg.websocket;
        if (key == "host") endpoint.host = value;
        else if (key == "port") endpoint.port = static_cast<std::uint16_t>(parse_int(value));
      } else if (section == "inference") {
        if (key == "enabled") cfg.inference.enabled = parse_bool(value);
        else if (key == "engine_path") cfg.inference.engine_path = value;
        else if (key == "window_size") cfg.inference.window_size = static_cast<std::size_t>(parse_int(value));
        else if (key == "stride") cfg.inference.stride = static_cast<std::size_t>(parse_int(value));
      } else if (section == "sensor_types") {
        if (indent == 2 && value.empty()) {
          sensor_type = key;
          SensorProfile profile;
          profile.name = sensor_type;
          cfg.sensor_types.emplace(sensor_type, std::move(profile));
        } else if (!sensor_type.empty()) {
          auto& profile = cfg.sensor_types.at(sensor_type);
          if (key == "frequency") profile.frequency_hz = parse_int(value);
          else if (key == "notify_handle") profile.notify_handle = static_cast<std::uint16_t>(parse_int(value));
          else if (key == "cccd_handle") profile.cccd_handle = static_cast<std::uint16_t>(parse_int(value));
          else if (key == "write_handle") profile.write_handle = static_cast<std::uint16_t>(parse_int(value));
          else if (key == "command_handle") profile.command_handle = static_cast<std::uint16_t>(parse_int(value));
        }
      } else if (section == "devices") {
        if (content.rfind("- ", 0) == 0) {
          cfg.devices.emplace_back();
          device = &cfg.devices.back();
          std::tie(key, value) = key_value(content.substr(2));
        }
        if (!device) throw std::runtime_error("device field before list item");
        if (key == "mac") device->mac = normalize_mac(value);
        else if (key == "type") device->type = value;
      }
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": " + error.what());
  }
  cfg.validate();
  return cfg;
}

void AppConfig::validate() const {
  static const std::regex mac_pattern(R"(^([0-9A-F]{2}:){5}[0-9A-F]{2}$)");
  if (tcp.port == 0 || websocket.port == 0) throw std::runtime_error("listen port must be in 1..65535");
  if (!std::regex_match(gateway_mac, mac_pattern)) throw std::runtime_error("invalid gateway_mac: " + gateway_mac);
  if (battery_interval_seconds <= 0 || reconnect_interval_seconds <= 0 || retention_days < 0)
    throw std::runtime_error("intervals must be positive and retention_days non-negative");
  if (inference.window_size == 0 || inference.stride == 0) throw std::runtime_error("inference window and stride must be positive");
  if (sensor_types.empty()) throw std::runtime_error("sensor_types cannot be empty");
  for (const auto& [name, profile] : sensor_types) {
    if (profile.frequency_hz <= 0 || profile.notify_handle == 0 || profile.cccd_handle == 0 || profile.write_handle == 0)
      throw std::runtime_error("incomplete sensor profile: " + name);
  }
  std::unordered_map<std::string, bool> seen;
  for (const auto& device : devices) {
    if (!std::regex_match(device.mac, mac_pattern)) throw std::runtime_error("invalid device MAC: " + device.mac);
    if (!seen.emplace(device.mac, true).second) throw std::runtime_error("duplicate device MAC: " + device.mac);
    if (sensor_types.find(device.type) == sensor_types.end()) throw std::runtime_error("unknown device type: " + device.type);
  }
}

const SensorProfile& AppConfig::profile_for_mac(const std::string& mac) const {
  const auto normalized = normalize_mac(mac);
  const auto device = std::find_if(devices.begin(), devices.end(), [&](const auto& item) { return item.mac == normalized; });
  if (device == devices.end()) throw std::out_of_range("device is not configured: " + normalized);
  return sensor_types.at(device->type);
}

bool AppConfig::is_allowed_device(const std::string& mac) const {
  const auto normalized = normalize_mac(mac);
  return std::any_of(devices.begin(), devices.end(), [&](const auto& device) { return device.mac == normalized; });
}

}  // namespace iot
