#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "iot/config.hpp"
#include "iot/json.hpp"
#include "iot/log.hpp"
#include "iot/services/data_pipeline.hpp"
#include "iot/services/database_writer.hpp"
#include "iot/services/health_monitor.hpp"
#include "iot/services/inference_worker.hpp"
#include "iot/services/tcp_server.hpp"
#include "iot/services/websocket_server.hpp"

namespace {
volatile std::sig_atomic_t interrupted = 0;  // 信号标志位
void signal_handler(int) { interrupted = true; }

std::string device_list_json(const iot::AppConfig& config) {
  /*发送websocket 初始化信息 ， 握手后立刻发送
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
    out << "{\"id\":\"" << iot::json_escape(config.devices[i].id) << "\",\"mac\":\""
        << iot::json_escape(config.devices[i].mac) << "\",\"type\":\""
        << iot::json_escape(config.devices[i].type) << "\"}";
  }
  out << "]}";
  return out.str();
}

const char* state_label(iot::ConnectionState state) {
  using S = iot::ConnectionState;
  switch (state) {
    case S::disconnected:
      return "已断开";
    case S::disconnecting:
      return "断开中";
    case S::searching:
      return "搜索中";
    case S::connecting:
      return "连接中";
    case S::connected:
      return "已连接";
    case S::enabling_notifications:
      return "启用通知中";
    case S::ready:
      return "就绪";
    default:
      return "未知";
  }
}

const iot::DeviceConfig* resolve_device(const iot::AppConfig& config, std::string target) {
  /*把用户在命令行里输入的设备标识符（可能是 ID 或MAC，大小写、前导零都可能不规范）解析成配置里对应的
   DeviceConfig 指针*/
  std::transform(target.begin(), target.end(), target.begin(),
                 [](unsigned char c) { return std::toupper(c); });  // 把 mac地址变成大写

  for (const auto& device : config.devices) {
    std::string id = device.id;
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::toupper(c); });
    if (device.mac == target || id == target) return &device;
  }
  // Accept 5 for 005, but never silently choose among ambiguous numeric IDs.
  const auto numeric = [](const std::string& value) {
    return !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
  };
  const auto normalized = [](const std::string& value) {
    const auto first = value.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : value.substr(first);
  };
  const iot::DeviceConfig* match = nullptr;
  if (numeric(target))
    for (const auto& device : config.devices) {
      if (numeric(device.id) && normalized(device.id) == normalized(target)) {
        if (match) return nullptr;
        match = &device;
      }
    }
  return match;
}

void print_gateways(const iot::services::TcpServer& server, const iot::AppConfig& config,
                    const iot::services::DataPipeline& pipeline) {
  std::ostringstream out;
  const auto snapshots = server.snapshots();
  const auto batteries = pipeline.batteries();
  out << "\n网关与设备状态\n" << std::string(90, '-') << '\n';
  if (snapshots.empty()) out << "暂无网关连接。以下为已配置设备及本次服务运行期间的最近电量。\n";
  for (const auto& gateway : snapshots)
    out << "网关 " << gateway.id << "  " << gateway.remote_address << (gateway.active ? " [在线]" : " [离线]")
        << '\n';
  for (const auto& device : config.devices) {
    out << "\nID=" << (device.id.empty() ? "-" : device.id) << "  MAC=" << device.mac
        << "  类型=" << device.type << (device.enabled ? "" : " [已禁用]") << '\n';
    bool found = false;
    for (const auto& gateway : snapshots) {
      if (!gateway.active) continue;
      const auto linked = std::find_if(gateway.connected.begin(), gateway.connected.end(),
                                       [&](const auto& item) { return item.mac == device.mac; });
      const auto scanned = std::find_if(gateway.discovered.begin(), gateway.discovered.end(),
                                        [&](const auto& item) { return item.mac == device.mac; });
      if (linked != gateway.connected.end() || scanned != gateway.discovered.end()) {
        found = true;
        out << "  网关=" << gateway.id << "  状态="
            << (linked != gateway.connected.end() ? state_label(linked->state) : "未连接（曾扫描到）");
        if (scanned != gateway.discovered.end()) {
          const auto scan_time = iot::Clock::to_time_t(scanned->last_seen);
          std::tm scan_tm{};
          localtime_r(&scan_time, &scan_tm);
          const auto scan_age = std::max<std::int64_t>(
              0, std::chrono::duration_cast<std::chrono::seconds>(iot::Clock::now() - scanned->last_seen)
                     .count());
          out << "  最近扫描 RSSI=" << scanned->rssi << " dBm"
              << "  扫描时间=" << std::put_time(&scan_tm, "%F %T") << "（本机时间，" << scan_age << " 秒前）";
        }
        out << '\n';
      }
    }
    if (!found) out << "  状态=未连接 / 暂无状态\n";
    const auto battery = batteries.find(device.mac);
    if (battery == batteries.end())
      out << "  最近电量=暂无记录\n";
    else {
      const auto time = iot::Clock::to_time_t(battery->second.received_at);
      std::tm tm{};
      localtime_r(&time, &tm);
      const auto age = std::max<std::int64_t>(
          0, std::chrono::duration_cast<std::chrono::seconds>(iot::Clock::now() - battery->second.received_at)
                 .count());
      out << "  最近电量=" << battery->second.level << "%  更新时间=" << std::put_time(&tm, "%F %T")
          << "（本机时间，" << age << " 秒前）\n";
    }
  }
  out << "\n共 " << config.devices.size() << " 台配置设备；就绪不代表持续收到采样。\n"
      << "电量每 " << config.battery_interval_seconds << " 秒查询；离线时保留旧值，服务重启后重新获取。\n";
  std::lock_guard<std::mutex> lock(iot::console_mutex());
  std::cout << out.str() << std::flush;
}

void print_command_menu() {
  std::cout << "\n"
            << std::string(60, '=') << '\n'
            << "可用命令：\n"
            << "  list / ls                    - 查看状态、最近电量和更新时间\n"
            << "  devices                      - 查看设备 ID 与 MAC 对照表\n"
            << "  gateway-info <网关ID>         - 查询过滤、扫描和固件信息\n"
            << "  scan-on <网关ID>              - 开启扫描输出并查询信息\n"
            << "  stats                        - 查看数据管道和队列状态\n"
            << "  connect <网关ID> <设备ID或MAC>   - 连接指定传感器\n"
            << "    示例：connect 0 005（也可输入 5 或完整 MAC）\n"
            << "  disconnect <网关ID> <设备ID或MAC> - 断开指定传感器\n"
            << "    示例：disconnect 0 005\n"
            << "  help / h / ?                 - 再次显示帮助\n"
            << "  q / quit / exit              - 停止服务并退出\n"
            << std::string(60, '=') << '\n';
}

void print_command_prompt() { std::cout << "iot> " << std::flush; }

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
      {
        std::lock_guard<std::mutex> lock(iot::console_mutex());
        print_command_menu();
        std::cout << "电量查询间隔：" << config.battery_interval_seconds << " 秒。";
        if (config.collection_schedule.enabled)
          std::cout << "采集时段：" << config.collection_schedule.start_time << "–"
                    << config.collection_schedule.stop_time << "（北京时间）。";
        std::cout << "\n提示：list 查看状态，connect 0 005 按 ID 连接设备。\n";
      }
      std::string line;
      while (!interrupted) {
        {
          std::lock_guard<std::mutex> lock(iot::console_mutex());
          print_command_prompt();
        }
        // Wait with a timeout so SIGINT/SIGTERM can leave an idle interactive CLI.
        // std::getline alone may restart its blocking read after the signal handler.
        pollfd input_descriptor{STDIN_FILENO, POLLIN, 0};
        while (!interrupted && poll(&input_descriptor, 1, 200) == 0) {
        }
        if (interrupted) break;
        if (!std::getline(std::cin, line)) break;

        std::istringstream input(line);
        std::string command, mac;
        std::uint64_t gateway{};
        input >> command;
        if (command.empty()) continue;
        std::transform(command.begin(), command.end(), command.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        // Keep each CLI response together with respect to background logs.
        // Network command dispatch can log, so it is deliberately outside this lock.

        if (command == "q" || command == "quit" || command == "exit") {
          std::cout << "正在停止服务，请稍候...\n";
          break;
        }
        if (command == "help" || command == "h" || command == "?") {
          print_command_menu();
          continue;
        }
        if (command == "list" || command == "ls") {
          print_gateways(tcp, config, pipeline);
          continue;
        }
        if (command == "devices") {
          std::lock_guard<std::mutex> lock(iot::console_mutex());
          std::cout << "ID\tMAC\t\t\t类型\n";
          for (const auto& device : config.devices)
            std::cout << (device.id.empty() ? "-" : device.id) << '\t' << device.mac << '\t' << device.type
                      << '\n';
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
            std::cerr << "网关不存在，请使用 list 查看网关 ID。\n";
          else
            std::cout << "查询请求已排队：网关 ID=" << gateway << "，结果将显示在后续日志中。\n";
          continue;
        }
        if (command == "connect" || command == "disconnect") {
          if (!(input >> gateway >> mac)) {
            std::cerr << "命令格式不完整。\n"
                      << "正确格式：" << command << " <网关ID> <设备ID或MAC>\n"
                      << "示例：" << command << " 0 3F:B8:12:EE:3C:37\n";
            continue;
          }

          std::string extra;
          if (input >> extra) {
            std::cerr << "参数过多：" << extra << "\n"
                      << "请使用：" << command << " <网关ID> <设备ID或MAC>\n";
            continue;
          }

          const auto* device = resolve_device(config, mac);
          if (!device) {
            std::cerr << "设备未配置或 ID 不明确：" << mac << "。输入 devices 查看 ID 与 MAC。\n";
            continue;
          }
          mac = device->mac;
          if (command == "connect" && !device->enabled) {
            std::cerr << "设备已禁用：ID=" << device->id << " MAC=" << mac << "，请修改配置后重启。\n";
            continue;
          }
          if (command == "connect" && !config.collection_schedule.allows(iot::Clock::now())) {
            std::cerr << "当前不在采集时段，无法连接：ID=" << device->id << " MAC=" << mac << "；北京时间 "
                      << config.collection_schedule.start_time << "–" << config.collection_schedule.stop_time
                      << "。\n";
            continue;
          }
          const bool success =
              command == "connect" ? tcp.connect_device(gateway, mac) : tcp.disconnect_device(gateway, mac);
          if (success) {
            std::cout << (command == "connect" ? "连接" : "断开") << "请求已排队：网关 ID=" << gateway
                      << "，设备 ID=" << device->id << " MAC=" << mac << '\n';
            std::cout << "请用 list 查看执行后的实际状态。\n";
            if (command == "disconnect" && config.collection_schedule.enabled &&
                config.collection_schedule.allows(iot::Clock::now()))
              std::cout << "该设备本采集时段暂停自动重连；下一采集时段恢复。\n";
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
