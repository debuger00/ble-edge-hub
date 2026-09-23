#include "iot/protocol/gateway_codec.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace iot::protocol {
namespace {

constexpr std::uint8_t kGatewayHead0 = 0xac;
constexpr std::uint8_t kGatewayHead1 = 0xca;
constexpr std::uint8_t kScanHead0 = 0xad;
constexpr std::uint8_t kScanHead1 = 0xba;

std::uint16_t be16(const std::uint8_t* data) { return static_cast<std::uint16_t>((data[0] << 8U) | data[1]); }

void append_be16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

}  // namespace

std::uint8_t xor_checksum(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint8_t result = 0;
  for (std::size_t i = 0; i < size; ++i) result ^= data[i];
  return result;
}

std::string format_mac(const std::uint8_t* bytes) {
  // 6 个字节的 MAC 地址转换成字符串
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  for (int i = 0; i < 6; ++i) {
    if (i) out << ':';  // 在第一个字节前不加冒号，后面的每个字节前都加一个冒号。
    out << std::setw(2) << static_cast<unsigned>(bytes[i]);  // setw(n:2) 设置输出宽度至少为
  }
  return out.str();
}

Bytes parse_mac(const std::string& mac) {
  Bytes result;
  std::istringstream in(mac);
  std::string byte;
  while (std::getline(in, byte, ':')) {
    if (byte.size() != 2) throw std::invalid_argument("invalid MAC: " + mac);
    std::size_t used{};
    const auto value = std::stoul(byte, &used, 16);
    if (used != 2 || value > 255) throw std::invalid_argument("invalid MAC: " + mac);
    result.push_back(static_cast<std::uint8_t>(value));
  }
  if (result.size() != 6) throw std::invalid_argument("invalid MAC: " + mac);
  return result;
}

StreamDecoder::StreamDecoder(DecoderCallbacks callbacks, std::size_t max_frame_size)
    : callbacks_(std::move(callbacks)), max_frame_size_(max_frame_size) {
  buffer_.reserve(max_frame_size);
}

void StreamDecoder::append(const std::uint8_t* data,
                           std::size_t size) {  // 这里是TCPserver 处理消息的主要入口
  if (!data && size) throw std::invalid_argument("null stream data");
  buffer_.insert(buffer_.end(), data, data + size);
  while (consume_one()) {
  }
  if (buffer_.size() > max_frame_size_) {
    report("receive buffer exceeded configured frame limit; resynchronizing");
    buffer_.clear();
  }
}

bool StreamDecoder::consume_one() {
  if (buffer_.size() < 2) return false;
  if (buffer_[0] == kGatewayHead0 && buffer_[1] == kGatewayHead1) return consume_gateway_frame();
  if (buffer_[0] == kScanHead0 && buffer_[1] == kScanHead1) return consume_scan_frame();
  buffer_.erase(buffer_.begin());
  report("discarded byte while searching for a frame header");
  return true;
}

bool StreamDecoder::consume_gateway_frame() {
  //处理数据帧
  if (buffer_.size() < 4) return false;
  const auto length = static_cast<std::size_t>(be16(buffer_.data() + 2));
  if (length < 14 || length > max_frame_size_) {
    report("invalid gateway frame length: " + std::to_string(length));
    buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
    return true;
  }
  if (buffer_.size() < length) return false;
  const auto expected = xor_checksum(buffer_.data(), length - 1);
  if (buffer_[length - 1] != expected) {
    report("gateway frame checksum mismatch");
    buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
    return true;
  }
  GatewayFrame frame;
  frame.gateway_mac = format_mac(buffer_.data() + 4);
  frame.message_id = be16(buffer_.data() + 10);
  frame.type = buffer_[12];
  frame.payload.assign(buffer_.begin() + 13, buffer_.begin() + static_cast<std::ptrdiff_t>(length - 1));
  if (callbacks_.on_gateway_frame) callbacks_.on_gateway_frame(frame);  // 这里是回调函数
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(length));
  return true;
}

bool StreamDecoder::consume_scan_frame() {
  if (buffer_.size() < 9) return false;
  const auto length = static_cast<std::size_t>(7 + be16(buffer_.data() + 5));
  if (length < 21 || length > max_frame_size_) {
    report("invalid scan frame length: " + std::to_string(length));
    buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
    return true;
  }
  if (buffer_.size() < length) return false;
  const auto checksum = xor_checksum(buffer_.data() + 5, length - 7);
  const auto high = buffer_[length - 2];
  const auto low = buffer_[length - 1];
  if (!((high == checksum && low == checksum) || (high == 0 && low == checksum))) {
    report("scan frame checksum mismatch");
    buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
    return true;
  }
  const auto type = buffer_[4];
  const auto count = be16(buffer_.data() + 17);
  const std::size_t block_size = type == 0x02 ? 69 : 38;
  std::size_t offset = 19;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (offset + block_size > length - 2) {
      report("scan frame device count exceeds payload");
      break;
    }
    const int rssi_value =
        buffer_[offset + 6] < 128 ? buffer_[offset + 6] : static_cast<int>(buffer_[offset + 6]) - 256;
    ScanEvent event{format_mac(buffer_.data() + offset), static_cast<std::int8_t>(rssi_value)};
    if ((!callbacks_.allow_scan_device || callbacks_.allow_scan_device(event.mac)) && callbacks_.on_scan)
      callbacks_.on_scan(event);
    offset += block_size;
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(length));
  return true;
}

void StreamDecoder::report(std::string message) const {
  if (callbacks_.on_error) callbacks_.on_error(message);
}

Bytes create_command(const std::string& gateway_mac, std::uint8_t type, std::uint16_t message_id,
                     const Bytes& payload) {  //这里网关发送mac 地址
  const auto length = 14U + payload.size();
  if (length > 65535) throw std::length_error("gateway command is too large");
  Bytes frame{kGatewayHead0, kGatewayHead1};
  append_be16(frame, static_cast<std::uint16_t>(length));
  const auto mac = parse_mac(gateway_mac);
  frame.insert(frame.end(), mac.begin(), mac.end());
  append_be16(frame, message_id);
  frame.push_back(type);
  frame.insert(frame.end(), payload.begin(), payload.end());
  frame.push_back(xor_checksum(frame.data(), frame.size()));
  return frame;
}

Bytes create_connect_command(const std::string& gateway_mac, std::uint16_t message_id,
                             const std::string& target_mac, std::uint16_t timeout_seconds,
                             std::uint16_t notify_handle, std::uint16_t cccd_handle) {
  Bytes payload{1};
  const auto mac = parse_mac(target_mac);
  payload.insert(payload.end(), mac.begin(), mac.end());
  append_be16(payload, timeout_seconds);
  payload.push_back(1);
  append_be16(payload, notify_handle);
  append_be16(payload, cccd_handle);
  return create_command(gateway_mac, 0x10, message_id, payload);
}

Bytes create_disconnect_command(const std::string& gateway_mac, std::uint16_t message_id,
                                const std::string& target_mac) {
  Bytes payload{1};
  const auto mac = parse_mac(target_mac);
  payload.insert(payload.end(), mac.begin(), mac.end());
  return create_command(gateway_mac, 0x11, message_id, payload);
}

Bytes create_send_data_command(const std::string& gateway_mac, std::uint16_t message_id,
                               const std::string& target_mac, std::uint16_t write_handle, const Bytes& data) {
  if (data.size() > 255) throw std::length_error("BLE write payload exceeds one-byte length");
  Bytes payload{1};
  const auto mac = parse_mac(target_mac);
  payload.insert(payload.end(), mac.begin(), mac.end());
  append_be16(payload, write_handle);
  payload.push_back(0);  // write without response
  payload.push_back(static_cast<std::uint8_t>(data.size()));
  payload.insert(payload.end(), data.begin(), data.end());
  return create_command(gateway_mac, 0x12, message_id, payload);
}

Bytes create_imu948_parameters_command(const std::string& gateway_mac, std::uint16_t message_id,
                                       const std::string& target_mac, std::uint16_t write_handle,
                                       int frequency_hz, std::uint16_t subscribe_tag) {
  const auto fps = static_cast<std::uint8_t>(std::clamp(frequency_hz, 1, 250));
  Bytes data{0x12,
             0x05,
             0xff,
             0x00,
             0x04,
             fps,
             0x01,
             0x03,
             0x05,
             static_cast<std::uint8_t>(subscribe_tag),
             static_cast<std::uint8_t>(subscribe_tag >> 8U)};
  return create_send_data_command(gateway_mac, message_id, target_mac, write_handle, data);
}

std::vector<Bytes> create_md9930_frequency_commands(const std::string& gateway_mac, std::uint16_t message_id,
                                                    const std::string& target_mac, std::uint16_t write_handle,
                                                    std::uint16_t command_handle, int frequency_hz) {
  const auto period = static_cast<std::uint16_t>(std::clamp(1000 / std::max(1, frequency_hz), 20, 1000));
  Bytes value{static_cast<std::uint8_t>(period >> 8U), static_cast<std::uint8_t>(period), 0, 0, 0, 0};
  return {create_send_data_command(gateway_mac, message_id, target_mac, write_handle, value),
          create_send_data_command(gateway_mac, static_cast<std::uint16_t>(message_id + 1), target_mac,
                                   command_handle, Bytes{0x0a})};
}

}  // namespace iot::protocol
