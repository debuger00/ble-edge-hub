#include "iot/protocol/websocket.hpp"

#include <regex>
#include <stdexcept>
#include <unordered_map>

#include "iot/config.hpp"

namespace iot::protocol {

void WebSocketDecoder::append(const std::uint8_t* data, std::size_t size,
                              const std::function<void(const WebSocketMessage&)>& handler) {
  buffer_.insert(buffer_.end(), data, data + size);
  while (buffer_.size() >= 2) {
    const auto first = buffer_[0];
    const auto opcode = static_cast<std::uint8_t>(first & 0x0f);
    const bool final = (first & 0x80) != 0;
    const bool control = opcode >= 8;
    if ((first & 0x70) || !(buffer_[1] & 0x80) ||
        (opcode != 0 && opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10))
      throw std::runtime_error("invalid WebSocket frame");
    std::size_t length = buffer_[1] & 0x7f;
    std::size_t header = 2;
    if (control && (!final || length > 125)) throw std::runtime_error("invalid WebSocket control frame");
    if (length == 127) throw std::runtime_error("WebSocket command too large");
    if (length == 126) {
      if (buffer_.size() < 4) return;
      length = (static_cast<std::size_t>(buffer_[2]) << 8) | buffer_[3];
      header = 4;
      if (length < 126) throw std::runtime_error("noncanonical WebSocket length");
    }
    if (length > 4096) throw std::runtime_error("WebSocket command too large");
    if (buffer_.size() < header + 4 + length) return;
    std::string payload(length, '\0');
    for (std::size_t i = 0; i < length; ++i)
      payload[i] = static_cast<char>(buffer_[header + 4 + i] ^ buffer_[header + i % 4]);
    buffer_.erase(buffer_.begin(), buffer_.begin() + header + 4 + length);
    if (control) {
      if (opcode == 8 && length == 1) throw std::runtime_error("invalid close frame");
      handler({opcode, std::move(payload)});
    } else {
      if ((opcode == 0) != fragment_open_) throw std::runtime_error("invalid WebSocket fragmentation");
      if (fragmented_.size() + payload.size() > 4096) throw std::runtime_error("WebSocket command too large");
      fragmented_ += payload;
      fragment_open_ = !final;
      if (final) {
        handler({1, std::move(fragmented_)});
        fragmented_.clear();
      }
    }
  }
}

WebSocketSubscription parse_subscription(const std::string& json) {
  // This endpoint accepts a small, flat JSON command, not arbitrary JSON.
  // MAC addresses and the fixed type need no escapes; reject other syntax.
  static const std::regex object(R"(^\s*\{([\s\S]*)\}\s*$)");
  static const std::regex member(
      R"re(\s*"(type|mac|sample_hz)"\s*:\s*("[^"\\\x00-\x1f]*"|0|[1-9][0-9]*)\s*)re");
  static const std::regex mac_pattern(R"(^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$)");
  std::smatch match;
  if (!std::regex_match(json, match, object)) throw std::runtime_error("expected subscription object");
  std::string remaining = match[1];
  std::unordered_map<std::string, std::string> fields;
  while (!remaining.empty()) {
    if (!std::regex_search(remaining, match, member, std::regex_constants::match_continuous))
      throw std::runtime_error("invalid subscription field");
    if (!fields.emplace(match[1].str(), match[2].str()).second)
      throw std::runtime_error("duplicate subscription field");
    remaining = match.suffix();
    if (remaining.empty()) break;
    if (remaining.front() != ',' || remaining.size() == 1)
      throw std::runtime_error("invalid subscription separator");
    remaining.erase(0, 1);
  }
  if (fields["type"] != "\"subscribe\"") throw std::runtime_error("expected subscribe command");
  const auto& value = fields["mac"];
  if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    throw std::runtime_error("expected MAC string");
  WebSocketSubscription subscription;
  subscription.mac = value.substr(1, value.size() - 2);
  if (!subscription.mac.empty() && !std::regex_match(subscription.mac, mac_pattern))
    throw std::runtime_error("invalid MAC address");
  subscription.mac = normalize_mac(subscription.mac);
  if (fields.count("sample_hz")) {
    const auto& hz = fields.at("sample_hz");
    if (hz.empty() || hz.front() == '"' || hz.size() > 2) throw std::runtime_error("sample_hz must be 0..25");
    subscription.sample_hz = static_cast<unsigned>(std::stoul(hz));
    if (subscription.sample_hz > 25) throw std::runtime_error("sample_hz must be 0..25");
  }
  return subscription;
}

}  // namespace iot::protocol
