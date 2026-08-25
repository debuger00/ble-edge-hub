#pragma once

#include "iot/domain.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace iot::protocol {

struct GatewayFrame {
  std::string gateway_mac;
  std::uint16_t message_id{};
  std::uint8_t type{};
  Bytes payload;
};

struct DecoderCallbacks {
  std::function<void(const GatewayFrame&)> on_gateway_frame;
  std::function<void(const ScanEvent&)> on_scan;
  std::function<bool(const std::string&)> allow_scan_device;
  std::function<void(const std::string&)> on_error;
};

class StreamDecoder {
 public:
  explicit StreamDecoder(DecoderCallbacks callbacks, std::size_t max_frame_size = 8192);
  void append(const std::uint8_t* data, std::size_t size);
  void append(const Bytes& data) { append(data.data(), data.size()); }
  std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

 private:
  bool consume_one();
  bool consume_gateway_frame();
  bool consume_scan_frame();
  void report(std::string message) const;

  DecoderCallbacks callbacks_;
  std::size_t max_frame_size_;
  Bytes buffer_;
};

std::uint8_t xor_checksum(const std::uint8_t* data, std::size_t size) noexcept;
std::string format_mac(const std::uint8_t* bytes);
Bytes parse_mac(const std::string& mac);

Bytes create_command(const std::string& gateway_mac, std::uint8_t type,
                     std::uint16_t message_id, const Bytes& payload = {});
Bytes create_connect_command(const std::string& gateway_mac, std::uint16_t message_id,
                             const std::string& target_mac, std::uint16_t timeout_seconds,
                             std::uint16_t notify_handle, std::uint16_t cccd_handle);
Bytes create_disconnect_command(const std::string& gateway_mac, std::uint16_t message_id,
                                const std::string& target_mac);
Bytes create_send_data_command(const std::string& gateway_mac, std::uint16_t message_id,
                               const std::string& target_mac, std::uint16_t write_handle,
                               const Bytes& data);
Bytes create_imu948_parameters_command(const std::string& gateway_mac, std::uint16_t message_id,
                                       const std::string& target_mac, std::uint16_t write_handle,
                                       int frequency_hz, std::uint16_t subscribe_tag = 0x0fff);
std::vector<Bytes> create_md9930_frequency_commands(const std::string& gateway_mac,
                                                     std::uint16_t message_id,
                                                     const std::string& target_mac,
                                                     std::uint16_t write_handle,
                                                     std::uint16_t command_handle,
                                                     int frequency_hz);

}  // namespace iot::protocol

