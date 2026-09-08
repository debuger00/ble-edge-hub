#pragma once

#include "iot/concurrency/bounded_queue.hpp"
#include "iot/config.hpp"

#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iot::services {

class WebSocketServer {
 public:
  WebSocketServer(EndpointConfig endpoint, std::string initial_message,
                  std::size_t queue_capacity = 4096);
  ~WebSocketServer();
  WebSocketServer(const WebSocketServer&) = delete;
  WebSocketServer& operator=(const WebSocketServer&) = delete;

  void start();
  // Empty MAC is low-rate shared metadata; device streams require a subscription.
  bool broadcast(std::string message, std::string mac = {}, bool sample = false,
                 std::int64_t timestamp_ms = 0);
  void stop();
  std::uint16_t port() const noexcept { return endpoint_.port; }  // Actual port after start().

  std::size_t queued_messages() const { return outgoing_.size(); }
  std::size_t dropped_messages() const { return outgoing_.dropped(); }
  std::size_t queue_capacity() const noexcept { return outgoing_.capacity(); }

 private:
  struct Client;
  struct OutgoingMessage {
    std::string payload;
    std::string mac;
    bool sample{};
    std::int64_t timestamp_ms{};
  };
  void accept_loop();
  void handle_client(int socket);
  void send_loop();
  void broadcast_now(const OutgoingMessage& message);
  void report_drop(std::size_t dropped);

  EndpointConfig endpoint_;
  std::string initial_message_;
  std::atomic<bool> running_{false};
  int listen_socket_{-1};
  std::thread accept_thread_;
  concurrency::BoundedQueue<OutgoingMessage> outgoing_;
  std::thread sender_thread_;
  std::mutex clients_mutex_;
  std::vector<std::shared_ptr<Client>> clients_;
  std::mutex workers_mutex_;
  std::vector<std::future<void>> workers_;
};

}  // namespace iot::services
