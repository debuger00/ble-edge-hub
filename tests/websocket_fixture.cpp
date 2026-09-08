#include "iot/json.hpp"
#include "iot/services/data_pipeline.hpp"
#include "iot/services/database_writer.hpp"
#include "iot/services/inference_worker.hpp"
#include "iot/services/websocket_server.hpp"

#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  iot::services::DatabaseWriter database(argv[1], 0, 30000);
  iot::InferenceConfig config;
  config.enabled = false;
  iot::services::InferenceWorker inference(config, {}, 5000);
  iot::services::WebSocketServer websocket({"127.0.0.1", 0},
      R"({"type":"device_list","devices":[{"mac":"AA:BB:CC:DD:EE:01"},{"mac":"AA:BB:CC:DD:EE:02"}]})");
  iot::services::DataPipeline pipeline(database, inference, websocket);
  websocket.start();
  std::cout << websocket.port() << std::endl;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream input(line);
    std::string command, mac;
    input >> command >> mac;
    if (command == "stop") break;
    if (command == "sample") {
      std::int64_t timestamp;
      double value;
      input >> timestamp >> value;
      iot::SensorSample sample;
      sample.mac = mac;
      sample.device_type = "imu948";
      sample.accel = iot::Vec3{value, 2, 3};
      sample.received_at = iot::Clock::time_point(std::chrono::milliseconds(timestamp));
      pipeline.publish(sample);
    } else if (command == "prediction") {
      websocket.broadcast(iot::prediction_json(mac, "walking"), mac);
    } else if (command == "battery") {
      iot::SensorStatus status;
      status.mac = mac;
      status.battery_level = 80;
      pipeline.publish(status);
    } else if (command == "barrier") {
      websocket.broadcast(R"({"type":"barrier"})");
    }
  }
  websocket.stop();
  database.stop();
}
