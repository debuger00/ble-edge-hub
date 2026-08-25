#include "iot/protocol/sensor_parser.hpp"

#include <cstring>
#include <stdexcept>

namespace iot::protocol {
namespace {

constexpr double kAccelScale = 0.00478515625;
constexpr double kGyroScale = 0.06103515625;
constexpr double kMdGyroScale = 0.07;
constexpr double kMagScale = 0.15106201171875;

class Reader {
 public:
  explicit Reader(const Bytes& bytes, std::size_t offset = 0) : bytes_(bytes), offset_(offset) {}
  std::size_t remaining() const { return bytes_.size() - offset_; }
  std::uint8_t u8() { require(1); return bytes_[offset_++]; }
  std::uint16_t le16() { require(2); auto v = static_cast<std::uint16_t>(bytes_[offset_] | bytes_[offset_ + 1] << 8U); offset_ += 2; return v; }
  std::int16_t sle16() { auto raw = le16(); std::int16_t value{}; std::memcpy(&value, &raw, sizeof(value)); return value; }
  std::int16_t sbe16() {
    require(2);
    auto raw = static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset_]) << 8U | bytes_[offset_ + 1]);
    offset_ += 2;
    std::int16_t value{}; std::memcpy(&value, &raw, sizeof(value)); return value;
  }
  std::uint32_t le32() {
    require(4);
    const auto v = static_cast<std::uint32_t>(bytes_[offset_]) |
                   static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8U |
                   static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16U |
                   static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24U;
    offset_ += 4; return v;
  }
  void skip(std::size_t n) { require(n); offset_ += n; }
 private:
  void require(std::size_t n) const { if (n > remaining()) throw std::out_of_range("sensor payload truncated"); }
  const Bytes& bytes_;
  std::size_t offset_{};
};

Vec3 vec_sle16(Reader& reader, double scale) {
  return {reader.sle16() * scale, reader.sle16() * scale, reader.sle16() * scale};
}

std::string fixed_string(const Bytes& payload, std::size_t offset, std::size_t size) {
  auto end = offset;
  while (end < offset + size && payload[end] != 0) ++end;
  return std::string(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.begin() + static_cast<std::ptrdiff_t>(end));
}

}  // namespace

std::optional<SensorMessage> Imu948Parser::parse(const std::string& mac, const Bytes& payload) const {
  if (payload.empty()) return std::nullopt;
  try {
    if (payload[0] == 0x11) {
      Reader reader(payload, 1);
      const auto flags = reader.le16();
      SensorSample sample;
      sample.mac = mac;
      sample.device_type = "imu948";
      sample.sensor_uptime_ms = reader.le32();
      if (flags & 0x0001) sample.accel = vec_sle16(reader, kAccelScale);
      if (flags & 0x0002) sample.raw_accel = vec_sle16(reader, kAccelScale);
      if (flags & 0x0004) sample.gyro = vec_sle16(reader, kGyroScale);
      if (flags & 0x0008) sample.mag = vec_sle16(reader, kMagScale);
      if (flags & 0x0010) reader.skip(8);  // temperature + pressure(24) + height(24)
      if (flags & 0x0020) reader.skip(8);  // quaternion
      if (flags & 0x0040) reader.skip(6);  // Euler angles
      // 后续订阅位暂未暴露到统一数据模型。保留尾部原始数据，不猜测其长度，
      // 以免新固件扩展字段时把一条本来有效的帧误判为截断。
      return SensorMessage{std::move(sample)};
    }
    if (payload[0] == 0x10 && payload.size() >= 33) {
      SensorStatus status;
      status.mac = mac;
      status.battery_level = payload[12];
      status.battery_voltage_mv = static_cast<std::uint16_t>(payload[13] | payload[14] << 8U);
      status.firmware_version = fixed_string(payload, 21, 6);
      status.product_model = fixed_string(payload, 27, 6);
      return SensorMessage{std::move(status)};
    }
  } catch (const std::out_of_range&) {
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<SensorMessage> Md9930Parser::parse(const std::string& mac, const Bytes& payload) const {
  if (payload.size() < 14) return std::nullopt;
  try {
    Reader reader(payload);
    SensorSample sample;
    sample.mac = mac;
    sample.device_type = "md9930";
    sample.accel = vec_sle16(reader, kAccelScale);
    sample.gyro = Vec3{reader.sbe16() * kMdGyroScale, reader.sbe16() * kMdGyroScale,
                       reader.sbe16() * kMdGyroScale};
    sample.counter = reader.le16();
    if (reader.remaining() >= 4) sample.ticks = reader.le32();
    sample.sensor_uptime_ms = sample.ticks ? std::optional<std::uint64_t>(*sample.ticks / 100U) : std::nullopt;
    return SensorMessage{std::move(sample)};
  } catch (const std::out_of_range&) {
    return std::nullopt;
  }
}

}  // namespace iot::protocol
