#include "iot/config.hpp"
#include "iot/json.hpp"
#include "iot/log.hpp"
#include "iot/services/database_writer.hpp"
#include "iot/services/inference_engine.hpp"
#include "iot/services/tcp_server.hpp"
#include "iot/services/websocket_server.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = true; }

std::string device_list_json(const iot::AppConfig& config) {
  std::ostringstream out;
  out << "{\"type\":\"device_list\",\"devices\":[";
  for (std::size_t i = 0; i < config.devices.size(); ++i) {
    if (i) out << ',';
    out << "{\"mac\":\"" << iot::json_escape(config.devices[i].mac)
        << "\",\"type\":\"" << iot::json_escape(config.devices[i].type) << "\"}";
  }
  out << "]}";
  return out.str();
}

void print_gateways(const iot::services::TcpServer& server) {
  const auto snapshots = server.snapshots();
  if (snapshots.empty()) { std::cout << "当前没有网关连接。\n"; return; }
  for (const auto& gateway : snapshots) {
    std::cout << "网关 " << gateway.id << "  " << gateway.remote_address
              << (gateway.active ? " [在线]" : " [离线]") << '\n';
    for (const auto& device : gateway.discovered)
      std::cout << "  发现: " << device.mac << " RSSI=" << device.rssi << '\n';
    for (const auto& device : gateway.connected)
      std::cout << "  连接: " << device.mac << " state=" << static_cast<int>(device.state) << '\n';
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path config_path{"config/config.yaml"};
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help" || argument == "-h") {
        std::cout << "Usage: " << argv[0] << " [--config <path>]\n";
        return 0;
      }
      if (argument == "--config" && i + 1 < argc) config_path = argv[++i];
      else if (argument.rfind("--", 0) == 0) throw std::invalid_argument("unknown option: " + argument);
      else config_path = argument;  // backward-compatible positional config path
    }
    const auto config = iot::AppConfig::load(config_path);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    iot::services::DatabaseWriter database(config.data_dir, config.retention_days);
    iot::services::InferenceEngine inference(config.inference);
    iot::services::WebSocketServer websocket(config.websocket, device_list_json(config));
    iot::services::TcpServer tcp(config, database, websocket, inference);

    websocket.start();
    tcp.start();
    if (isatty(STDIN_FILENO)) {
      std::cout << "服务已启动。命令: list | connect <网关ID> <MAC> | disconnect <网关ID> <MAC> | q\n";
      std::string line;
      while (!interrupted && std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command, mac;
        std::uint64_t gateway{};
        input >> command;
        if (command == "q" || command == "quit" || command == "exit") break;
        if (command == "list") print_gateways(tcp);
        else if (command == "connect" && input >> gateway >> mac) {
          if (!tcp.connect_device(gateway, mac)) std::cerr << "连接命令失败：请检查网关 ID 和已配置 MAC。\n";
        } else if (command == "disconnect" && input >> gateway >> mac) {
          if (!tcp.disconnect_device(gateway, mac)) std::cerr << "断开命令失败：请检查网关 ID 和已配置 MAC。\n";
        } else if (!command.empty()) std::cerr << "无法识别的命令或参数不完整。\n";
      }
    } else {
      iot::log(iot::LogLevel::info, "running without interactive CLI; send SIGINT or SIGTERM to stop");
      while (!interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    tcp.stop();
    websocket.stop();
    database.stop();
    iot::log(iot::LogLevel::info, "all services stopped cleanly");
    return 0;
  } catch (const std::exception& error) {
    iot::log(iot::LogLevel::error, error.what());
    return 1;
  }
}
