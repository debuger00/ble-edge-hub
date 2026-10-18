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

std::size_t parse_positive_size(const std::string& text) {
  const auto value = parse_int(text);
  if (value <= 0) throw std::runtime_error("value must be positive: " + text);
  return static_cast<std::size_t>(value);
}

int minute_of_day(const std::string& text) {
  if (text.size() != 5 || text[2] != ':' ||
      !std::isdigit(static_cast<unsigned char>(text[0])) ||
      !std::isdigit(static_cast<unsigned char>(text[1])) ||
      !std::isdigit(static_cast<unsigned char>(text[3])) ||
      !std::isdigit(static_cast<unsigned char>(text[4])))
    throw std::runtime_error("schedule time must be HH:MM: " + text);
  const int hour = (text[0] - '0') * 10 + text[1] - '0';
  const int minute = (text[3] - '0') * 10 + text[4] - '0';
  if (hour > 23 || minute > 59) throw std::runtime_error("invalid schedule time: " + text);
  return hour * 60 + minute;
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
      // Preserve '#' inside quoted values (notably SMTP authorization codes).
      char quote = 0;
      for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (quote) {
          if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
          quote = c;
        } else if (c == '#') {
          raw.erase(i);
          break;
        }
      }
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
      } else if (section == "collection_schedule") {
        if (key == "enabled") cfg.collection_schedule.enabled = parse_bool(value);
        else if (key == "start_time") cfg.collection_schedule.start_time = value;
        else if (key == "stop_time") cfg.collection_schedule.stop_time = value;
        else throw std::runtime_error("unknown collection_schedule field: " + key);
      } else if (section == "alerts") {
        if (key == "enabled") cfg.alerts.enabled = parse_bool(value);
        else if (key == "timeout_seconds") cfg.alerts.timeout_seconds = parse_int(value);
        else if (key == "retry_interval_seconds") cfg.alerts.retry_interval_seconds = parse_int(value);
        else if (key == "smtp_timeout_seconds") cfg.alerts.smtp_timeout_seconds = parse_int(value);
        else if (key == "smtp_host") cfg.alerts.smtp_host = value;
        else if (key == "smtp_port") cfg.alerts.smtp_port = parse_int(value);
        else if (key == "smtp_security") cfg.alerts.smtp_security = value;
        else if (key == "smtp_username") cfg.alerts.smtp_username = value;
        else if (key == "smtp_password") cfg.alerts.smtp_password = value;
        else if (key == "from_email") cfg.alerts.from_email = value;
        else if (key == "to_email") cfg.alerts.to_email = value;
        else throw std::runtime_error("unknown alerts field: " + key);
      } else if (section == "queues") {
        if (key == "database_capacity") cfg.queues.database_capacity = parse_positive_size(value);
        else if (key == "inference_capacity") cfg.queues.inference_capacity = parse_positive_size(value);
        else if (key == "websocket_capacity") cfg.queues.websocket_capacity = parse_positive_size(value);
      } else if (section == "sensor_types") {
        if (indent == 2 && value.empty()) {
          sensor_type = key;
          SensorProfile profile;
          profile.name = sensor_type;
          cfg.sensor_types.emplace(sensor_type, std::move(profile));
        } else if (!sensor_type.empty()) {
          auto& profile = cfg.sensor_types.at(sensor_type);
          if (key == "frequency") profile.frequency_hz = parse_int(value);
          else if (key == "subscribe_tag") {
            const auto tag = parse_int(value);
            if (tag < 0 || tag > 0xffff) throw std::runtime_error("subscribe_tag must fit in 16 bits");
            profile.subscribe_tag = static_cast<std::uint16_t>(tag);
          }
          else if (key == "notify_handle") profile.notify_handle = static_cast<std::uint16_t>(parse_int(value));
          else if (key == "cccd_handle") profile.cccd_handle = static_cast<std::uint16_t>(parse_int(value));
          else if (key == "write_handle") profile.write_handle = static_cast<std::uint16_t>(parse_int(value));
          else if (key == "battery_handle") profile.battery_handle = static_cast<std::uint16_t>(parse_int(value));
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
        else if (key == "id") device->id = value;
      }
    }
  } catch (const std::exception& error) {
    throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": " + error.what());
  }
  cfg.validate();
  return cfg;
}

void CollectionSchedule::validate() const {
  if (minute_of_day(start_time) == minute_of_day(stop_time))
    throw std::runtime_error("schedule start_time and stop_time must differ");
}

bool CollectionSchedule::allows(std::chrono::system_clock::time_point now) const {
  if (!enabled) return true;
  // Beijing civil time is explicitly UTC+8, independent of process TZ.
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  const auto local_seconds = ((seconds + 8 * 3600) % 86400 + 86400) % 86400;
  const int minute = static_cast<int>(local_seconds / 60);
  const int start = minute_of_day(start_time), stop = minute_of_day(stop_time);
  return start < stop ? minute >= start && minute < stop : minute >= start || minute < stop;
}

void AppConfig::validate() const {
  collection_schedule.validate();
  if (alerts.timeout_seconds <= 0 || alerts.retry_interval_seconds <= 0 ||
      alerts.smtp_timeout_seconds <= 0 || alerts.smtp_timeout_seconds > 120)
    throw std::runtime_error("alert intervals must be positive; SMTP timeout must be <=120 seconds");
  if (alerts.smtp_port <= 0 || alerts.smtp_port > 65535)
    throw std::runtime_error("alerts.smtp_port must be in 1..65535");
  if (alerts.smtp_security != "ssl" && alerts.smtp_security != "starttls" && alerts.smtp_security != "none")
    throw std::runtime_error("alerts.smtp_security must be ssl, starttls or none");
  if (alerts.enabled) {
    static const std::regex email_pattern(R"(^[^\s<>@]+@[^\s<>@]+$)");
    if (alerts.smtp_host.empty() || !std::regex_match(alerts.from_email, email_pattern) ||
        !std::regex_match(alerts.to_email, email_pattern))
      throw std::runtime_error("enabled alerts require smtp_host, from_email and to_email (one address each)");
    if (alerts.smtp_username.empty() != alerts.smtp_password.empty())
      throw std::runtime_error("SMTP username and password must both be set or both empty");
  }
  static const std::regex mac_pattern(R"(^([0-9A-F]{2}:){5}[0-9A-F]{2}$)");
  if (tcp.port == 0 || websocket.port == 0) throw std::runtime_error("listen port must be in 1..65535");
  if (!std::regex_match(gateway_mac, mac_pattern)) throw std::runtime_error("invalid gateway_mac: " + gateway_mac);
  if (battery_interval_seconds <= 0 || reconnect_interval_seconds <= 0 || retention_days < 0)
    throw std::runtime_error("intervals must be positive and retention_days non-negative");
  if (inference.window_size == 0 || inference.stride == 0) throw std::runtime_error("inference window and stride must be positive");
  if (queues.database_capacity == 0 || queues.inference_capacity == 0 || queues.websocket_capacity == 0)
    throw std::runtime_error("queue capacities must be positive");
  if (sensor_types.empty()) throw std::runtime_error("sensor_types cannot be empty");
  for (const auto& [name, profile] : sensor_types) {
    if (profile.frequency_hz <= 0 || profile.notify_handle == 0 || profile.cccd_handle == 0 || profile.write_handle == 0)
      throw std::runtime_error("incomplete sensor profile: " + name);
  }
  std::unordered_map<std::string, bool> seen;
  std::unordered_map<std::string, bool> ids;
  for (const auto& device : devices) {
    if (alerts.enabled && device.id.empty()) throw std::runtime_error("alerts require an id for every device");
    if (!device.id.empty() && (!std::regex_match(device.id, std::regex("[A-Za-z0-9_-]{1,64}")) ||
                               !ids.emplace(device.id, true).second))
      throw std::runtime_error("device id must be unique, 1..64 letters/digits/underscore/hyphen: " + device.id);
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
