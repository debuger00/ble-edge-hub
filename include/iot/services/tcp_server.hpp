#pragma once

#include "iot/config.hpp"
#include "iot/domain.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace iot::services {

class DataPipeline;

struct DiscoveredDevice {
  std::string mac;
  int rssi{};
  Clock::time_point last_seen;
};

struct ConnectedDevice {
  std::string mac;
  ConnectionState state{ConnectionState::unknown};
  Clock::time_point last_update;
};

struct GatewaySnapshot {
  std::uint64_t id{};
  std::string remote_address;
  bool active{};
  std::vector<DiscoveredDevice> discovered;
  std::vector<ConnectedDevice> connected;
};

class TcpServer {
 public:
  TcpServer(const AppConfig& config, DataPipeline& pipeline);
  ~TcpServer();
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  void start();
  void stop();
  std::vector<GatewaySnapshot> snapshots() const;
  bool connect_device(std::uint64_t gateway_id, const std::string& mac);
  bool disconnect_device(std::uint64_t gateway_id, const std::string& mac);

 private:
  class Session;
  void accept_loop();

  const AppConfig& config_;
  DataPipeline& pipeline_;
  std::atomic<bool> running_{false};
  int listen_socket_{-1};
  std::thread accept_thread_;
  mutable std::mutex sessions_mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  std::uint64_t next_session_id_{};
};

}  // namespace iot::services
