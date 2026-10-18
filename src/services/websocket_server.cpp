#include "iot/services/websocket_server.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "iot/json.hpp"
#include "iot/log.hpp"
#include "iot/protocol/websocket.hpp"

namespace iot::services {
namespace {

std::uint32_t rotate_left(std::uint32_t value, unsigned amount) {
  return (value << amount) | (value >> (32U - amount));
}

std::array<std::uint8_t, 20> sha1(const std::string& input) {
  std::vector<std::uint8_t> message(input.begin(), input.end());
  const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8;
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0);
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<std::uint8_t>(bit_length >> (i * 8)));
  std::uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
  for (std::size_t offset = 0; offset < message.size(); offset += 64) {
    std::array<std::uint32_t, 80> words{};
    for (int i = 0; i < 16; ++i) {
      const auto p = offset + static_cast<std::size_t>(i) * 4;
      words[i] =
          static_cast<std::uint32_t>(message[p]) << 24U | static_cast<std::uint32_t>(message[p + 1]) << 16U |
          static_cast<std::uint32_t>(message[p + 2]) << 8U | static_cast<std::uint32_t>(message[p + 3]);
    }
    for (int i = 16; i < 80; ++i)
      words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    auto a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f{}, k{};
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }
      const auto temp = rotate_left(a, 5) + f + e + k + words[i];
      e = d;
      d = c;
      c = rotate_left(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  std::array<std::uint8_t, 20> digest{};
  const std::array<std::uint32_t, 5> words{h0, h1, h2, h3, h4};
  for (std::size_t i = 0; i < words.size(); ++i)
    for (int byte = 0; byte < 4; ++byte)
      digest[i * 4 + byte] = static_cast<std::uint8_t>(words[i] >> (24 - byte * 8));
  return digest;
}

std::string base64(const std::uint8_t* data, std::size_t size) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < size; i += 3) {
    const auto remaining = size - i;
    const std::uint32_t word =
        data[i] << 16U | (remaining > 1 ? data[i + 1] << 8U : 0) | (remaining > 2 ? data[i + 2] : 0);
    out.push_back(alphabet[(word >> 18U) & 63]);
    out.push_back(alphabet[(word >> 12U) & 63]);
    out.push_back(remaining > 1 ? alphabet[(word >> 6U) & 63] : '=');
    out.push_back(remaining > 2 ? alphabet[word & 63] : '=');
  }
  return out;
}

bool send_all(int socket, const std::uint8_t* data, std::size_t size) {
  while (size) {
    const auto sent = ::send(socket, data, size, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return false;
    data += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

std::vector<std::uint8_t> text_frame(const std::string& message) {
  std::vector<std::uint8_t> frame{0x81};
  if (message.size() <= 125)
    frame.push_back(static_cast<std::uint8_t>(message.size()));
  else if (message.size() <= 65535) {
    frame.push_back(126);
    frame.push_back(static_cast<std::uint8_t>(message.size() >> 8U));
    frame.push_back(static_cast<std::uint8_t>(message.size()));
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(message.size()) >> (i * 8)));
  }
  frame.insert(frame.end(), message.begin(), message.end());
  return frame;
}

int create_listener(const EndpointConfig& endpoint) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses{};
  const auto service = std::to_string(endpoint.port);
  if (getaddrinfo(endpoint.host.empty() ? nullptr : endpoint.host.c_str(), service.c_str(), &hints,
                  &addresses) != 0)
    throw std::runtime_error("cannot resolve WebSocket listen address");
  int listener = -1;
  for (auto* address = addresses; address; address = address->ai_next) {
    listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener < 0) continue;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 && listen(listener, 16) == 0) break;
    close(listener);
    listener = -1;
  }
  freeaddrinfo(addresses);
  if (listener < 0)
    throw std::runtime_error("cannot bind WebSocket endpoint " + endpoint.host + ':' + service);
  return listener;
}

}  // namespace

struct WebSocketServer::Client {
  explicit Client(int fd) : socket(fd) {}
  ~Client() { close(socket); }
  const int socket;
  std::atomic<bool> active{true};
  // Serializes subscription changes with complete frame writes.
  std::mutex mutex;
  protocol::WebSocketSubscription subscription;
  double next_sample_ms{};
  std::int64_t last_sample_ms{};

  bool send(const std::string& payload, std::uint8_t opcode = 1) {
    if (!active) return false;
    auto frame = text_frame(payload);
    frame[0] = 0x80 | opcode;
    if (send_all(socket, frame.data(), frame.size())) return true;
    disconnect();
    return false;
  }
  void disconnect() {
    active = false;
    shutdown(socket, SHUT_RDWR);
  }
};

WebSocketServer::WebSocketServer(EndpointConfig endpoint, std::string initial_message,
                                 std::size_t queue_capacity)  // 初始化函数
    : endpoint_(std::move(endpoint)),
      initial_message_(std::move(initial_message)),
      outgoing_(queue_capacity, concurrency::OverflowPolicy::drop_oldest) {}
WebSocketServer::~WebSocketServer() { stop(); }

void WebSocketServer::start() {
  if (running_.exchange(true)) return;
  try {
    listen_socket_ = create_listener(endpoint_);
  } catch (...) {
    running_ = false;
    throw;
  }
  if (endpoint_.port == 0) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
      endpoint_.port = address.ss_family == AF_INET
                           ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
                           : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
  }
  sender_thread_ = std::thread(&WebSocketServer::send_loop, this);
  accept_thread_ = std::thread(&WebSocketServer::accept_loop, this);
  log(LogLevel::info, "WebSocket listening on " + endpoint_.host + ':' + std::to_string(endpoint_.port) +
                          "; queue capacity=" + std::to_string(outgoing_.capacity()));
}

void WebSocketServer::accept_loop() {
  while (running_) {
    const int client = accept(listen_socket_, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (running_) log(LogLevel::error, "WebSocket accept failed");
      break;
    }
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                  [](auto& worker) {
                                    if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                                      return false;
                                    worker.get();
                                    return true;
                                  }),
                   workers_.end());
    workers_.push_back(std::async(std::launch::async, &WebSocketServer::handle_client, this, client));
  }
}

void WebSocketServer::handle_client(int socket) {
  auto client = std::make_shared<Client>(socket);
  timeval timeout{5, 0};
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  timeval send_timeout{1, 0};
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
  try {
    std::string request;
    std::array<char, 2048> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384 && running_) {
      const auto received = recv(socket, buffer.data(), buffer.size(), 0);
      if (received <= 0) return;
      request.append(buffer.data(), static_cast<std::size_t>(received));
    }
    const auto header_end = request.find("\r\n\r\n");
    const std::string header = "Sec-WebSocket-Key:";
    auto key_pos = request.find(header);
    if (header_end == std::string::npos || key_pos == std::string::npos || key_pos > header_end) return;
    key_pos += header.size();
    while (key_pos < request.size() && request[key_pos] == ' ') ++key_pos;
    const auto key_end = request.find("\r\n", key_pos);
    if (key_end == std::string::npos) return;
    const auto digest =
        sha1(request.substr(key_pos, key_end - key_pos) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const auto response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
        "Upgrade\r\nSec-WebSocket-Accept: " +
        base64(digest.data(), digest.size()) + "\r\n\r\n";
    if (!send_all(socket, reinterpret_cast<const std::uint8_t*>(response.data()), response.size())) return;
    if (!client->send(initial_message_)) return;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients_.push_back(client);
    }
    protocol::WebSocketDecoder decoder;
    auto handle = [&](const protocol::WebSocketMessage& message) {
      std::lock_guard<std::mutex> lock(client->mutex);
      if (!client->active) return;
      if (message.opcode == 8) {
        client->send(message.payload, 8);
        client->disconnect();
      } else if (message.opcode == 9) {
        client->send(message.payload, 10);
      } else if (message.opcode == 1) {
        try {
          client->subscription = protocol::parse_subscription(message.payload);
          client->next_sample_ms = 0;
          client->last_sample_ms = 0;
          client->send("{\"type\":\"subscribed\",\"mac\":\"" + json_escape(client->subscription.mac) +
                       "\",\"sample_hz\":" + std::to_string(client->subscription.sample_hz) + "}");
        } catch (const std::exception&) {
          client->send("{\"type\":\"error\",\"message\":\"Invalid subscribe command\"}");
        }
      }
    };
    const auto pending = request.substr(header_end + 4);
    decoder.append(reinterpret_cast<const std::uint8_t*>(pending.data()), pending.size(), handle);
    while (running_ && client->active) {
      pollfd descriptor{socket, POLLIN, 0};
      const int status = poll(&descriptor, 1, 200);
      if (status < 0 && errno == EINTR) continue;
      if (status < 0 || descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
      if (!(descriptor.revents & POLLIN)) continue;
      const auto received = recv(socket, buffer.data(), buffer.size(), 0);
      if (received <= 0) break;
      decoder.append(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(received),
                     handle);
    }
  } catch (const std::exception& error) {
    log(LogLevel::warning, std::string("WebSocket client closed: ") + error.what());
  }
  client->disconnect();
  std::lock_guard<std::mutex> lock(clients_mutex_);
  clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

bool WebSocketServer::broadcast(std::string message, std::string mac, bool sample,
                                std::int64_t timestamp_ms) {
  if (!running_) return false;
  const auto result =
      outgoing_.try_push({std::move(message), normalize_mac(std::move(mac)), sample, timestamp_ms});
  if (result == concurrency::PushResult::dropped_oldest) report_drop(outgoing_.dropped());
  return result == concurrency::PushResult::pushed || result == concurrency::PushResult::dropped_oldest;
}

void WebSocketServer::send_loop() {
  // A low-rate activity channel keeps unselected devices visible without
  // broadcasting their full sensor streams. Silence expires in the browser.
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> activity_sent;
  while (auto message = outgoing_.wait_pop()) {
    if (!running_) break;
    if (message->sample) {
      const auto now = std::chrono::steady_clock::now();
      auto found = activity_sent.find(message->mac);
      if (found == activity_sent.end() || now - found->second >= std::chrono::seconds(1)) {
        activity_sent[message->mac] = now;
        const auto age = std::max<std::int64_t>(0, epoch_millis() - message->timestamp_ms);
        broadcast_now({"{\"type\":\"device_activity\",\"mac\":\"" + json_escape(message->mac) +
                           "\",\"age_ms\":" + std::to_string(age) + "}",
                       {},
                       false,
                       0});
      }
    }
    broadcast_now(*message);
  }
}

void WebSocketServer::broadcast_now(const OutgoingMessage& message) {
  std::vector<std::shared_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients = clients_;
  }
  for (const auto& client : clients) {
    std::lock_guard<std::mutex> lock(client->mutex);
    if (!client->active) continue;
    if (!message.mac.empty() && client->subscription.mac != message.mac) continue;
    if (message.sample && client->subscription.sample_hz) {
      const double now = static_cast<double>(message.timestamp_ms);
      // Timestamp regression (clock correction) restarts the display limiter.
      if (message.timestamp_ms < client->last_sample_ms) client->next_sample_ms = 0;
      client->last_sample_ms = message.timestamp_ms;
      if (now < client->next_sample_ms) continue;
      const double interval = 1000.0 / client->subscription.sample_hz;
      if (client->next_sample_ms == 0 || now - client->next_sample_ms >= interval)
        client->next_sample_ms = now;
      client->next_sample_ms += interval;
    }
    client->send(message.payload);
  }
}

void WebSocketServer::report_drop(std::size_t dropped) {
  if (dropped == 1 || dropped % 1000 == 0) {
    log(LogLevel::warning, "WebSocket queue overloaded; dropped oldest messages=" + std::to_string(dropped));
  }
}

void WebSocketServer::stop() {
  if (!running_.exchange(false)) return;
  if (listen_socket_ >= 0) {
    shutdown(listen_socket_, SHUT_RDWR);
    close(listen_socket_);
    listen_socket_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& client : clients_) client->disconnect();
  }
  outgoing_.close();
  if (sender_thread_.joinable()) sender_thread_.join();
  {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto& worker : workers_) worker.get();
    workers_.clear();
  }
  std::lock_guard<std::mutex> lock(clients_mutex_);
  clients_.clear();
}

}  // namespace iot::services
