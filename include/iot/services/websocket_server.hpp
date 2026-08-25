#pragma once

#include "iot/config.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iot::services {

class WebSocketServer {
 public:
  WebSocketServer(EndpointConfig endpoint, std::string initial_message);
  ~WebSocketServer();
  WebSocketServer(const WebSocketServer&) = delete;
  WebSocketServer& operator=(const WebSocketServer&) = delete;

  void start();
  void broadcast(const std::string& message);
  void stop();

 private:
  void accept_loop();
  void handle_client(int socket);

  EndpointConfig endpoint_;
  std::string initial_message_;
  std::atomic<bool> running_{false};
  int listen_socket_{-1};
  std::thread accept_thread_;
  std::mutex clients_mutex_;
  std::vector<int> clients_;
  std::mutex workers_mutex_;
  std::vector<std::thread> workers_;
};

}  // namespace iot::services
