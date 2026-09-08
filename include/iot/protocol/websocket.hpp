#pragma once

#include "iot/domain.hpp"

#include <functional>
#include <string>

namespace iot::protocol {

struct WebSocketMessage {
  std::uint8_t opcode;
  std::string payload;
};

// Client frames must be masked. Supports fragmented text and control frames;
// caps each command/message at 4096 bytes to bound per-client memory.
class WebSocketDecoder {
 public:
  void append(const std::uint8_t* data, std::size_t size,
              const std::function<void(const WebSocketMessage&)>& handler);

 private:
  Bytes buffer_;
  std::string fragmented_;
  bool fragment_open_{};
};

struct WebSocketSubscription {
  std::string mac;  // Empty means no device stream.
  unsigned sample_hz{};  // Zero preserves every sample; 1..25 limits display traffic.
};

WebSocketSubscription parse_subscription(const std::string& json);

}  // namespace iot::protocol
