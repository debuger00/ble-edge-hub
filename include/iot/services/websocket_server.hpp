#pragma once

#include "iot/concurrency/bounded_queue.hpp"
#include "iot/config.hpp"

#include <atomic>
#include <cstddef>
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
  bool broadcast(std::string message);
  void stop();

  std::size_t queued_messages() const { return outgoing_.size(); }
  std::size_t dropped_messages() const { return outgoing_.dropped(); }
  std::size_t queue_capacity() const noexcept { return outgoing_.capacity(); }

 private:
  void accept_loop();
  void handle_client(int socket);
  void send_loop();
  void broadcast_now(const std::string& message);
  void report_drop(std::size_t dropped);

  EndpointConfig endpoint_;
  std::string initial_message_;
  std::atomic<bool> running_{false};
  int listen_socket_{-1};
  std::thread accept_thread_;
  concurrency::BoundedQueue<std::string> outgoing_;
  std::thread sender_thread_;
  std::mutex clients_mutex_;
  std::vector<int> clients_;
  std::mutex workers_mutex_;
  std::vector<std::thread> workers_;
};

}  // namespace iot::services
