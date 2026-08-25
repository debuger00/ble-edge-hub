#pragma once

#include "iot/domain.hpp"

#include <optional>
#include <string>
#include <variant>

namespace iot::protocol {

using SensorMessage = std::variant<SensorSample, SensorStatus>;

class ISensorParser {
 public:
  virtual ~ISensorParser() = default;
  virtual std::optional<SensorMessage> parse(const std::string& mac, const Bytes& payload) const = 0;
};

class Imu948Parser final : public ISensorParser {
 public:
  std::optional<SensorMessage> parse(const std::string& mac, const Bytes& payload) const override;
};

class Md9930Parser final : public ISensorParser {
 public:
  std::optional<SensorMessage> parse(const std::string& mac, const Bytes& payload) const override;
};

}  // namespace iot::protocol

