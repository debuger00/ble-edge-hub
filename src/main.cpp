#include <unistd.h>

#include <atomic>
#include <csignal>
#include <poll.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "iot/config.hpp"
#include "iot/json.hpp"
#include "iot/log.hpp"
#include "iot/services/data_pipeline.hpp"
#include "iot/services/health_monitor.hpp"
#include "iot/services/database_writer.hpp"
#include "iot/services/inference_worker.hpp"
#include "iot/services/tcp_server.hpp"
#include "iot/services/websocket_server.hpp"

namespace {
volatile std::sig_atomic_t interrupted = 0;  // 信号标志位
void signal_handler(int) { interrupted = true; }

std::string device_list_json(const iot::AppConfig& config) {
  /*发送websocket 初始化信息
  {
  "type": "device_list",
  "devices": [
    {
      "mac": "AA:BB:CC:DD:EE:01",
      "type": "sensor"
    },
    {
      "mac": "AA:BB:CC:DD:EE:02",
      "type": "switch"
    }
  ]
  }
  */
  std::ostringstream out;
  out << "{\"type\":\"device_list\",\"devices\":[";
  for (std::size_t i = 0; i < config.devices.size(); ++i) {
    if (i) out << ',';
    out << "{\"id\":\"" << iot::json_escape(config.devices[i].id) << "\",\"mac\":\"" << iot::json_escape(config.devices[i].mac) << "\",\"type\":\""
        << iot::json_escape(config.devices[i].type) << "\"}";
  }
  out << "]}";
  return out.str();
}

void print_gateways(const iot::services::TcpServer& server) {
  const auto snapshots = server.snapshots();
  if (snapshots.empty()) {
    std::cout << "当前没有网关连接，请稍候再使用 list 刷新。\n";
    return;
  }
  for (const auto& gateway : snapshots) {
    std::cout << "网关 " << gateway.id << "  " << gateway.remote_address
              << (gateway.active ? " [在线]" : " [离线]") << '\n';
    std::cout << "=========================================" << '\n';
    for (const auto& device : gateway.discovered)
      std::cout << "  发现: " << device.mac << " RSSI=" << device.rssi << '\n';
    std::cout << "=========================================" << '\n';
    for (const auto& device : gateway.connected)
      std::cout << "  连接: " << device.mac << " state=" << static_cast<int>(device.state) << '\n';
  }
}

void print_command_menu() {
  std::cout << "\n"
            << std::string(60, '=') << '\n'
            << "可用命令：\n"
            << "  list                         - 查看网关和设备状态\n"
            << "  devices                      - 查看设备 ID 与 MAC 对照表\n"
            << "  gateway-info <网关ID>         - 查询过滤、扫描和固件信息\n"
            << "  scan-on <网关ID>              - 开启扫描输出并查询信息\n"
            << "  stats                        - 查看数据管道和队列状态\n"
            << "  connect <网关ID> <设备MAC>   - 连接指定传感器\n"
            << "    示例：connect 0 3F:B8:12:EE:3C:37\n"
            << "  disconnect <网关ID> <设备MAC> - 断开指定传感器\n"
            << "    示例：disconnect 0 3F:B8:12:EE:3C:37\n"
            << "  help / h / ?                 - 再次显示帮助\n"
            << "  q / quit / exit              - 停止服务并退出\n"
            << std::string(60, '=') << '\n';
}

void print_command_prompt() { std::cout << "请输入命令（输入 help 查看帮助）> " << std::flush; }

void print_pipeline_stats(const iot::services::DataPipeline& pipeline,
                          const iot::services::DatabaseWriter& database,
                          const iot::services::InferenceWorker& inference,
                          const iot::services::WebSocketServer& websocket) {
  std::cout << "数据管道状态：\n"
            << "  数据库队列   " << database.queued() << '/' << database.capacity()
            << "，拒绝=" << pipeline.database_dropped() << '\n'
            << "  推理队列     " << inference.queued() << '/' << inference.capacity()
            << "，丢弃最旧=" << inference.dropped() << "，拒绝=" << pipeline.inference_rejected() << '\n'
            << "  WebSocket队列 " << websocket.queued_messages() << '/' << websocket.queue_capacity()
            << "，丢弃最旧=" << websocket.dropped_messages() << "，拒绝=" << pipeline.websocket_rejected()
            << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path config_path{"config/config.yaml"};  //默认是 config/config.yaml 文件
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help" || argument == "-h") {
        std::cout << "Usage: " << argv[0] << " [--config <path>]\n";
        return 0;
      }
      if (argument == "--config" && i + 1 < argc)
        config_path = argv[++i];
      else if (argument.rfind("--", 0) == 0)  //
        throw std::invalid_argument("unknown option: " + argument);
      else
        config_path = argument;  // backward-compatible positional config path
    }
    const auto config = iot::AppConfig::load(config_path);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    iot::services::DatabaseWriter database(config.data_dir, config.retention_days,
                                           config.queues.database_capacity);
    iot::services::WebSocketServer websocket(config.websocket, device_list_json(config),
                                             config.queues.websocket_capacity);
    iot::services::InferenceWorker inference(
        config.inference,
        [&websocket](const std::string& mac, const std::string& action) {
          websocket.broadcast(iot::prediction_json(mac, action), mac);
        },
        config.queues.inference_capacity);
    //这里是注册回调函数，捕获外部变量 websocket
    //对应 函数原型InferenceWorker(const InferenceConfig& config, PredictionHandler
    // prediction_handler,  std::size_t queue_capacity);

    iot::services::HealthMonitor health(config);
    iot::services::DataPipeline pipeline(database, inference, websocket, &health);
    iot::services::TcpServer tcp(config, pipeline);

    websocket.start();
    inference.start();
    tcp.start();
    health.start();
    if (isatty(STDIN_FILENO)) {
      std::cout << "服务已启动。\n";
      print_command_menu();
      std::string line;
      while (!interrupted) {
        print_command_prompt();
        // Wait with a timeout so SIGINT/SIGTERM can leave an idle interactive CLI.
        // std::getline alone may restart its blocking read after the signal handler.
        pollfd input_descriptor{STDIN_FILENO, POLLIN, 0};
        while (!interrupted && poll(&input_descriptor, 1, 200) == 0) {}
        if (interrupted) break;
        if (!std::getline(std::cin, line)) break;

        std::istringstream input(line);
        std::string command, mac;
        std::uint64_t gateway{};
        input >> command;
        if (command.empty()) {
          std::cout << "未输入命令，请输入 list、stats、connect、disconnect、help 或 q。\n";
          continue;
        }

        if (command == "q" || command == "quit" || command == "exit") {
          std::cout << "正在停止服务，请稍候...\n";
          break;
        }
        if (command == "help" || command == "h" || command == "?") {
          print_command_menu();
          continue;
        }
        if (command == "list") {
          print_gateways(tcp);
          continue;
        }
        if (command == "devices") {
          std::cout << "ID\tMAC\t\t\t类型\n";
          for (const auto& device : config.devices)
            std::cout << (device.id.empty() ? "-" : device.id) << '\t' << device.mac << '\t' << device.type << '\n';
          continue;
        }
        if (command == "stats") {
          print_pipeline_stats(pipeline, database, inference, websocket);
          continue;
        }

        if (command == "gateway-info" || command == "scan-on") {
          std::string extra;
          if (!(input >> gateway) || (input >> extra)) {
            std::cerr << "用法：" << command << " <网关ID>\n";
            continue;
          }
          if (!tcp.inspect_gateway(gateway, command == "scan-on"))
            std::cerr << "网关不存在\n";
          continue;
        }
        if (command == "connect" || command == "disconnect") {
          if (!(input >> gateway >> mac)) {
            std::cerr << "命令格式不完整。\n"
                      << "正确格式：" << command << " <网关ID> <设备MAC>\n"
                      << "示例：" << command << " 0 3F:B8:12:EE:3C:37\n";
            continue;
          }

          std::string extra;
          if (input >> extra) {
            std::cerr << "参数过多：" << extra << "\n"
                      << "请使用：" << command << " <网关ID> <设备MAC>\n";
            continue;
          }

          const bool success =
              command == "connect" ? tcp.connect_device(gateway, mac) : tcp.disconnect_device(gateway, mac);
          if (success) {
            std::cout << (command == "connect" ? "连接" : "断开") << "命令已发送：网关 ID=" << gateway
                      << "，设备 MAC=" << mac << '\n';
          } else {
            std::cerr << (command == "connect" ? "连接" : "断开")
                      << "命令发送失败。请确认网关 ID、设备 MAC 已配置，并使用 list 查看状态。\n";
          }
          continue;
        }

        std::cerr << "未知命令：" << command << '\n' << "请输入 help 查看可用命令。\n";
      }
    } else {
      iot::log(iot::LogLevel::info, "running without interactive CLI; send SIGINT or SIGTERM to stop");
      while (!interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    health.stop();
    tcp.stop();  //逐层 停止， 和 实例化  start 顺序相反。
    inference.stop();
    websocket.stop();
    database.stop();
    iot::log(iot::LogLevel::info, "all services stopped cleanly");
    return 0;
  } catch (const std::exception& error) {
    iot::log(iot::LogLevel::error, error.what());
    return 1;
  }
}
