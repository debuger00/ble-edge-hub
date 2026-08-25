#include "iot/services/websocket_server.hpp"

#include "iot/log.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>

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
      words[i] = static_cast<std::uint32_t>(message[p]) << 24U |
                 static_cast<std::uint32_t>(message[p + 1]) << 16U |
                 static_cast<std::uint32_t>(message[p + 2]) << 8U |
                 static_cast<std::uint32_t>(message[p + 3]);
    }
    for (int i = 16; i < 80; ++i) words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    auto a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f{}, k{};
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
      else { f = b ^ c ^ d; k = 0xca62c1d6; }
      const auto temp = rotate_left(a, 5) + f + e + k + words[i];
      e = d; d = c; c = rotate_left(b, 30); b = a; a = temp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  std::array<std::uint8_t, 20> digest{};
  const std::array<std::uint32_t, 5> words{h0, h1, h2, h3, h4};
  for (std::size_t i = 0; i < words.size(); ++i)
    for (int byte = 0; byte < 4; ++byte) digest[i * 4 + byte] = static_cast<std::uint8_t>(words[i] >> (24 - byte * 8));
  return digest;
}

std::string base64(const std::uint8_t* data, std::size_t size) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < size; i += 3) {
    const auto remaining = size - i;
    const std::uint32_t word = data[i] << 16U | (remaining > 1 ? data[i + 1] << 8U : 0) | (remaining > 2 ? data[i + 2] : 0);
    out.push_back(alphabet[(word >> 18U) & 63]); out.push_back(alphabet[(word >> 12U) & 63]);
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
    data += sent; size -= static_cast<std::size_t>(sent);
  }
  return true;
}

std::vector<std::uint8_t> text_frame(const std::string& message) {
  std::vector<std::uint8_t> frame{0x81};
  if (message.size() <= 125) frame.push_back(static_cast<std::uint8_t>(message.size()));
  else if (message.size() <= 65535) {
    frame.push_back(126); frame.push_back(static_cast<std::uint8_t>(message.size() >> 8U)); frame.push_back(static_cast<std::uint8_t>(message.size()));
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(message.size()) >> (i * 8)));
  }
  frame.insert(frame.end(), message.begin(), message.end());
  return frame;
}

int create_listener(const EndpointConfig& endpoint) {
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses{};
  const auto service = std::to_string(endpoint.port);
  if (getaddrinfo(endpoint.host.empty() ? nullptr : endpoint.host.c_str(), service.c_str(), &hints, &addresses) != 0)
    throw std::runtime_error("cannot resolve WebSocket listen address");
  int listener = -1;
  for (auto* address = addresses; address; address = address->ai_next) {
    listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener < 0) continue;
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 && listen(listener, 16) == 0) break;
    close(listener); listener = -1;
  }
  freeaddrinfo(addresses);
  if (listener < 0) throw std::runtime_error("cannot bind WebSocket endpoint " + endpoint.host + ':' + service);
  return listener;
}

}  // namespace

WebSocketServer::WebSocketServer(EndpointConfig endpoint, std::string initial_message)
    : endpoint_(std::move(endpoint)), initial_message_(std::move(initial_message)) {}
WebSocketServer::~WebSocketServer() { stop(); }

void WebSocketServer::start() {
  if (running_.exchange(true)) return;
  try { listen_socket_ = create_listener(endpoint_); }
  catch (...) { running_ = false; throw; }
  accept_thread_ = std::thread(&WebSocketServer::accept_loop, this);
  log(LogLevel::info, "WebSocket listening on " + endpoint_.host + ':' + std::to_string(endpoint_.port));
}

void WebSocketServer::accept_loop() {
  while (running_) {
    const int client = accept(listen_socket_, nullptr, nullptr);
    if (client < 0) { if (errno == EINTR) continue; if (running_) log(LogLevel::error, "WebSocket accept failed"); break; }
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.emplace_back(&WebSocketServer::handle_client, this, client);
  }
}

void WebSocketServer::handle_client(int client) {
  timeval timeout{5, 0}; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::string request;
  std::array<char, 2048> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
    const auto received = recv(client, buffer.data(), buffer.size(), 0);
    if (received <= 0) { close(client); return; }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  const std::string header = "Sec-WebSocket-Key:";
  auto key_pos = request.find(header);
  if (key_pos == std::string::npos) { close(client); return; }
  key_pos += header.size();
  while (key_pos < request.size() && request[key_pos] == ' ') ++key_pos;
  const auto key_end = request.find("\r\n", key_pos);
  if (key_end == std::string::npos) { close(client); return; }
  const auto digest = sha1(request.substr(key_pos, key_end - key_pos) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  const auto response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                        base64(digest.data(), digest.size()) + "\r\n\r\n";
  if (!send_all(client, reinterpret_cast<const std::uint8_t*>(response.data()), response.size())) { close(client); return; }
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.push_back(client);
    const auto frame = text_frame(initial_message_);
    if (!send_all(client, frame.data(), frame.size())) { clients_.pop_back(); close(client); }
  }
}

void WebSocketServer::broadcast(const std::string& message) {
  const auto frame = text_frame(message);
  std::lock_guard<std::mutex> lock(clients_mutex_);
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](int client) {
    if (send_all(client, frame.data(), frame.size())) return false;
    close(client); return true;
  }), clients_.end());
}

void WebSocketServer::stop() {
  if (!running_.exchange(false)) return;
  if (listen_socket_ >= 0) { shutdown(listen_socket_, SHUT_RDWR); close(listen_socket_); listen_socket_ = -1; }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
  }
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (const auto client : clients_) { shutdown(client, SHUT_RDWR); close(client); }
  clients_.clear();
}

}  // namespace iot::services
