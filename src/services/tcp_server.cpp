#include "iot/services/tcp_server.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "iot/log.hpp"
#include "iot/protocol/gateway_codec.hpp"
#include "iot/protocol/sensor_parser.hpp"
#include "iot/services/data_pipeline.hpp"

namespace iot::services {
namespace {

// 创建并返回一个监听 socket，绑定到 endpoint 指定的地址与端口。
// 成功时返回监听描述符；无法解析或绑定地址时抛出 std::runtime_error。
int create_listener(const EndpointConfig& endpoint) {
  // 配置 getaddrinfo 的查询约束：不限定协议族（IPv4/IPv6 均可），
  // 使用面向连接的 TCP 流，并以被动监听（AI_PASSIVE）方式解析地址。
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses{};
  const auto service = std::to_string(endpoint.port);
  // 解析主机名与端口号。host 为空时传 nullptr，表示绑定到任意本地地址。
  if (getaddrinfo(endpoint.host.empty() ? nullptr : endpoint.host.c_str(), service.c_str(), &hints,
                  &addresses) != 0)
    throw std::runtime_error("cannot resolve TCP listen address");
  int listener = -1;
  // 依次尝试 getaddrinfo 返回的每个候选地址，直到成功 bind + listen。
  for (auto* address = addresses; address; address = address->ai_next) {
    listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener < 0) continue;
    // 设置 SO_REUSEADDR，允许快速重启时复用处于 TIME_WAIT 的本地端口。
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // 绑定并开始监听（backlog = 16）。成功则保留该 socket 并跳出循环。
    if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 && listen(listener, 16) == 0) break;
    close(listener);
    listener = -1;
  }
  // 释放 getaddrinfo 分配的结果链表。
  freeaddrinfo(addresses);
  if (listener < 0) throw std::runtime_error("cannot bind TCP endpoint " + endpoint.host + ':' + service);
  return listener;
}

std::string peer_name(int socket) {
  sockaddr_storage address{};
  socklen_t size = sizeof(address);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return "unknown";
  char host[NI_MAXHOST]{}, service[NI_MAXSERV]{};
  if (getnameinfo(reinterpret_cast<sockaddr*>(&address), size, host, sizeof(host), service, sizeof(service),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    return "unknown";
  return std::string(host) + ':' + service;
}

std::uint16_t next_message_id() {
  static std::atomic<std::uint16_t> id{1};
  return id.fetch_add(1, std::memory_order_relaxed);
}

std::string connection_name(ConnectionState state) {
  switch (state) {
    case ConnectionState::disconnected:
      return "disconnected";
    case ConnectionState::disconnecting:
      return "disconnecting";
    case ConnectionState::searching:
      return "searching";
    case ConnectionState::connecting:
      return "connecting";
    case ConnectionState::connected:
      return "connected";
    case ConnectionState::enabling_notifications:
      return "enabling notifications";
    case ConnectionState::ready:
      return "ready";
    default:
      return "unknown";
  }
}

}  // namespace

// class TcpServer {} 是定义类的整体结构，不能在头文件和 .cpp
// 中各写一次；TcpServer::start() 是在类定义结束后，实现其中已经声明的成员函数。::
// 正是 C++ 用于分离类声明与函数实现的语法。

class TcpServer::Session {
 public:
  Session(std::uint64_t id, int socket, const AppConfig& config, DataPipeline& pipeline)
      : id_(id),
        socket_(socket),
        remote_(peer_name(socket)),
        config_(config),
        pipeline_(pipeline),
        decoder_({[this](const protocol::GatewayFrame& frame) { on_frame(frame); },
                  [this](const ScanEvent& event) { on_scan(event); },
                  [this](const std::string& mac) { return config_.is_allowed_device(mac); },
                  [this](const std::string& error) { log(LogLevel::warning, remote_ + ": " + error); }}) {}
  ~Session() { stop(); }
  void start() {
    active_ = true;
    worker_ = std::thread(&Session::run, this);
  }
  void stop() {
    if (active_.exchange(false)) shutdown(socket_, SHUT_RDWR);
    if (worker_.joinable()) worker_.join();
    if (socket_ >= 0) {
      close(socket_);
      socket_ = -1;
    }
  }
  void queue(Bytes command) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (active_) outgoing_.push_back(std::move(command));
  }
  bool connect_device(const std::string& mac) {
    try {
      const auto normalized = normalize_mac(mac);
      const auto& profile = config_.profile_for_mac(normalized);
      queue(protocol::create_connect_command(config_.gateway_mac, next_message_id(), normalized, 30,
                                             profile.notify_handle, profile.cccd_handle));
      return true;
    } catch (const std::exception& error) {
      log(LogLevel::error, error.what());
      return false;
    }
  }
  bool disconnect_device(const std::string& mac) {
    const auto normalized = normalize_mac(mac);
    if (!config_.is_allowed_device(normalized)) return false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      reconnect_.erase(normalized);
    }
    queue(protocol::create_disconnect_command(config_.gateway_mac, next_message_id(), normalized));
    return true;
  }
  GatewaySnapshot snapshot() const {
    GatewaySnapshot result;
    result.id = id_;
    result.remote_address = remote_;
    result.active = active_;
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& [_, item] : discovered_) result.discovered.push_back(item);
    for (const auto& [_, item] : connected_) result.connected.push_back(item);
    return result;
  }

 private:
  void run() {
    log(LogLevel::info, "gateway connected: " + remote_ + " (id=" + std::to_string(id_) + ')');
    auto next_battery =
        std::chrono::steady_clock::now() + std::chrono::seconds(config_.battery_interval_seconds);
    auto next_reconnect =
        std::chrono::steady_clock::now() + std::chrono::seconds(config_.reconnect_interval_seconds);
    std::array<std::uint8_t, 4096> incoming{};
    while (active_) {
      pollfd descriptor{socket_, POLLIN, 0};
      const int status = poll(&descriptor, 1, 100);
      if (status < 0 && errno == EINTR) continue;
      if (status < 0 || descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
      if (descriptor.revents & POLLIN) {
        const auto count = recv(socket_, incoming.data(), incoming.size(), 0);
        if (count <= 0) break;
        decoder_.append(incoming.data(), static_cast<std::size_t>(count));
      }
      Bytes command;
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!outgoing_.empty()) {
          command = std::move(outgoing_.front());
          outgoing_.pop_front();
        }
      }
      if (!command.empty() && !send_all(command)) break;
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_battery) {
        query_batteries();
        next_battery = now + std::chrono::seconds(config_.battery_interval_seconds);
      }
      if (now >= next_reconnect) {
        reconnect_devices();
        next_reconnect = now + std::chrono::seconds(config_.reconnect_interval_seconds);
      }
    }
    active_ = false;
    shutdown(socket_, SHUT_RDWR);
    log(LogLevel::warning, "gateway disconnected: " + remote_);
  }
  bool send_all(const Bytes& command) {
    auto* data = command.data();
    auto remaining = command.size();
    while (remaining) {
      const auto sent = send(socket_, data, remaining, MSG_NOSIGNAL);
      if (sent < 0 && errno == EINTR) continue;
      if (sent <= 0) return false;
      data += sent;
      remaining -= static_cast<std::size_t>(sent);
    }
    return true;
  }
  void on_scan(const ScanEvent& event) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    discovered_[event.mac] = DiscoveredDevice{event.mac, event.rssi, Clock::now()};
  }
  void on_frame(const protocol::GatewayFrame& frame) {
    if (frame.type == 0x20)
      handle_device_data(frame.payload);
    else if (frame.type == 0x21)
      handle_connection(frame.payload);
    else if (frame.type == 0x22 && frame.payload.size() != 8)
      log(LogLevel::warning, "invalid heartbeat payload length");
    else if (frame.type == 0xf0 && frame.payload.size() >= 2 && frame.payload[1] != 0)
      log(LogLevel::warning, "gateway command failed, type=" + std::to_string(frame.payload[0]));
    else if (frame.type != 0x22 && frame.type != 0x27 && frame.type != 0xf0)
      log(LogLevel::warning, "unsupported gateway frame type=" + std::to_string(frame.type));
  }
  void handle_device_data(const Bytes& payload) {
    if (payload.size() < 9) {
      log(LogLevel::warning, "device data payload is too short");
      return;
    }
    const auto length = static_cast<std::size_t>(payload[8]);
    if (payload.size() != 9 + length) {
      log(LogLevel::warning, "device data length mismatch");
      return;
    }
    const auto mac = protocol::format_mac(payload.data());
    if (!config_.is_allowed_device(mac)) {
      log(LogLevel::warning, "ignored unconfigured device: " + mac);
      return;
    }
    const auto handle = static_cast<std::uint16_t>(payload[6] << 8U | payload[7]);
    const auto& profile = config_.profile_for_mac(mac);
    if (handle != profile.notify_handle) log(LogLevel::debug, "data arrived on unexpected handle for " + mac);
    const Bytes sensor_payload(payload.begin() + 9, payload.end());
    std::optional<protocol::SensorMessage> parsed;
    if (profile.name == "imu948")
      parsed = protocol::Imu948Parser{}.parse(mac, sensor_payload);
    else if (profile.name == "md9930")
      parsed = protocol::Md9930Parser{}.parse(mac, sensor_payload);
    else {
      log(LogLevel::warning, "no parser registered for sensor type: " + profile.name);
      return;
    }
    if (!parsed) {
      log(LogLevel::warning, "malformed sensor payload from " + mac);
      return;
    }
    if (auto* sample = std::get_if<SensorSample>(&*parsed)) {
      pipeline_.publish(std::move(*sample));
    } else if (auto* status = std::get_if<SensorStatus>(&*parsed)) {
      pipeline_.publish(*status);
    }
  }
  void handle_connection(const Bytes& payload) {
    if (payload.size() != 7) {
      log(LogLevel::warning, "invalid connection event length");
      return;
    }
    const auto mac = protocol::format_mac(payload.data());
    if (!config_.is_allowed_device(mac)) return;
    const auto raw = payload[6];
    const auto state = raw <= 6 ? static_cast<ConnectionState>(raw) : ConnectionState::unknown;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state == ConnectionState::disconnected)
        connected_.erase(mac);
      else
        connected_[mac] = ConnectedDevice{mac, state, Clock::now()};
      if (state == ConnectionState::connected || state == ConnectionState::ready) reconnect_.insert(mac);
    }
    log(LogLevel::info, mac + " connection state: " + connection_name(state));
    if (state == ConnectionState::ready) configure_ready_device(mac);
  }
  void configure_ready_device(const std::string& mac) {
    const auto& profile = config_.profile_for_mac(mac);
    auto id = next_message_id();
    queue(protocol::create_send_data_command(config_.gateway_mac, id++, mac, profile.write_handle,
                                             Bytes{0x29}));
    if (profile.name == "md9930") {
      const auto command_handle = profile.command_handle.value_or(profile.write_handle);
      for (auto& command : protocol::create_md9930_frequency_commands(
               config_.gateway_mac, id, mac, profile.write_handle, command_handle, profile.frequency_hz))
        queue(std::move(command));
    } else {
      queue(protocol::create_imu948_parameters_command(config_.gateway_mac, id, mac, profile.write_handle,
                                                       profile.frequency_hz));
    }
  }
  void query_batteries() {
    std::vector<std::string> devices;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (const auto& [mac, _] : connected_) devices.push_back(mac);
    }
    for (const auto& mac : devices) {
      const auto& profile = config_.profile_for_mac(mac);
      // IMU948 supports status tag 0x10. MD9930 battery is a GATT read and cannot use this write command.
      if (profile.name == "imu948")
        queue(protocol::create_send_data_command(config_.gateway_mac, next_message_id(), mac,
                                                 profile.write_handle, Bytes{0x10}));
    }
  }
  void reconnect_devices() {
    std::vector<std::string> targets;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (const auto& mac : reconnect_)
        if (connected_.find(mac) == connected_.end()) targets.push_back(mac);
    }
    for (const auto& mac : targets) connect_device(mac);
  }

  std::uint64_t id_;
  int socket_;
  std::string remote_;
  const AppConfig& config_;
  DataPipeline& pipeline_;
  protocol::StreamDecoder decoder_;
  std::atomic<bool> active_{false};
  std::thread worker_;
  mutable std::mutex state_mutex_;
  std::unordered_map<std::string, DiscoveredDevice> discovered_;
  std::unordered_map<std::string, ConnectedDevice> connected_;
  std::unordered_set<std::string> reconnect_;
  std::mutex queue_mutex_;
  std::deque<Bytes> outgoing_;
};

TcpServer::TcpServer(const AppConfig& config, DataPipeline& pipeline)
    : config_(config), pipeline_(pipeline) {}
TcpServer::~TcpServer() { stop(); }

void TcpServer::start() {
  if (running_.exchange(true)) return;
  try {
    listen_socket_ = create_listener(config_.tcp);
  } catch (...) {
    running_ = false;
    throw;
  }
  accept_thread_ = std::thread(&TcpServer::accept_loop, this);
  log(LogLevel::info,
      "TCP gateway server listening on " + config_.tcp.host + ':' + std::to_string(config_.tcp.port));
}

void TcpServer::accept_loop() {
  while (running_) {
    const int client = accept(listen_socket_, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (running_) log(LogLevel::error, "TCP accept failed");
      break;
    }
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto id = next_session_id_++;
    auto session = std::make_shared<Session>(id, client, config_, pipeline_);
    sessions_[id] = session;
    session->start();
  }
}

void TcpServer::stop() {
  if (!running_.exchange(false)) return;
  if (listen_socket_ >= 0) {
    shutdown(listen_socket_, SHUT_RDWR);
    close(listen_socket_);
    listen_socket_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (const auto& [_, session] : sessions_) sessions.push_back(session);
    sessions_.clear();
  }
  for (const auto& session : sessions) session->stop();
}

std::vector<GatewaySnapshot> TcpServer::snapshots() const {
  std::vector<GatewaySnapshot> result;
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  for (const auto& [_, session] : sessions_) result.push_back(session->snapshot());
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  return result;
}

bool TcpServer::connect_device(std::uint64_t gateway_id, const std::string& mac) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  const auto found = sessions_.find(gateway_id);
  return found != sessions_.end() && found->second->connect_device(mac);
}

bool TcpServer::disconnect_device(std::uint64_t gateway_id, const std::string& mac) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  const auto found = sessions_.find(gateway_id);
  return found != sessions_.end() && found->second->disconnect_device(mac);
}

}  // namespace iot::services
