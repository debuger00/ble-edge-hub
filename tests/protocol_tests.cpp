#include "iot/config.hpp"
#include "iot/concurrency/bounded_queue.hpp"
#include "iot/protocol/gateway_codec.hpp"
#include "iot/protocol/sensor_parser.hpp"
#include "iot/protocol/websocket.hpp"
#include "iot/services/inference_engine.hpp"
#include "iot/services/inference_worker.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

void test_gateway_command_and_stream_decoder() {
  const auto command = iot::protocol::create_send_data_command(
      "D6:EA:A6:E2:B0:70", 0x1234, "CE:43:75:25:96:BF", 0x0006, {0x29});
  expect(command.size() == 26, "unexpected send-data command size");
  expect(command[0] == 0xac && command[1] == 0xca, "wrong command header");
  expect(iot::protocol::xor_checksum(command.data(), command.size() - 1) == command.back(), "wrong checksum");

  int frames = 0;
  iot::protocol::GatewayFrame decoded;
  iot::protocol::StreamDecoder decoder({[&](const auto& frame) { decoded = frame; ++frames; }, {}, {}, {}});
  decoder.append(command.data(), 3);
  expect(frames == 0, "partial frame must not emit");
  decoder.append(command.data() + 3, command.size() - 3);
  expect(frames == 1 && decoded.type == 0x12 && decoded.message_id == 0x1234, "gateway frame decode failed");
}

void test_config() {
  const auto config = iot::AppConfig::load(std::filesystem::path(IOT_TEST_SOURCE_DIR) / "config/config.yaml");
  expect(config.profile_for_mac("54:6C:0E:B8:37:E4").battery_handle == 0x000f, "battery handle config missing");
  expect(config.tcp.port == 7628, "TCP config was not loaded");
  expect(config.queues.database_capacity == 30000, "queue config was not loaded");
  expect(config.profile_for_mac("54:6c:0e:b9:1a:4c").name == "md9930", "MAC normalization failed");
  bool rejected = false;
  try { (void)config.profile_for_mac("00:00:00:00:00:00"); } catch (const std::out_of_range&) { rejected = true; }
  expect(rejected, "unknown MAC unexpectedly got a fallback profile");
}

void test_collection_schedule() {
  iot::CollectionSchedule schedule;
  schedule.enabled = true;
  const auto utc = [](int hour, int minute, int second = 0) {
    return std::chrono::system_clock::time_point{} +
        std::chrono::hours(hour) + std::chrono::minutes(minute) + std::chrono::seconds(second);
  };
  expect(!schedule.allows(utc(5, 59, 59)), "must wait until Beijing 14:00");
  expect(schedule.allows(utc(6, 0)), "start must be inclusive");
  expect(schedule.allows(utc(8, 59, 59)), "must collect before stop");
  expect(!schedule.allows(utc(9, 0)), "stop must be exclusive");
  expect(schedule.allows(utc(30, 0)), "must repeat next day");
  expect(schedule.allows(utc(7, 0)), "restart or clock correction inside window");
  expect(!schedule.allows(utc(5, 0)), "backward clock correction outside window");
  schedule.start_time = "22:00";
  schedule.stop_time = "02:00";
  expect(schedule.allows(utc(14, 0)), "overnight start");
  expect(schedule.allows(utc(16, 0)), "Beijing midnight");
  expect(schedule.allows(utc(17, 59, 59)), "overnight before stop");
  expect(!schedule.allows(utc(18, 0)), "overnight stop");
  expect(!schedule.allows(utc(13, 59, 59)), "overnight before start");
  for (const auto* invalid : {"24:00", "14:60", "2:00", "14:00:00", "xx:yy", "02:00"}) {
    schedule.start_time = invalid;
    bool rejected = false;
    try { schedule.validate(); } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, "invalid or equal schedule times must fail");
  }
  schedule.enabled = false;
  expect(schedule.allows(utc(0, 0)), "disabled schedule must preserve manual mode");
  // Parsing tests must not depend on the operator's live schedule switch.
  const auto fixture = std::filesystem::temp_directory_path() /
      ("iot-schedule-config-" + std::to_string(getpid()) + ".yaml");
  {
    std::ofstream out(fixture);
    out << "collection_schedule:\n  enabled: true\n  start_time: \"14:00\"\n  stop_time: \"17:00\"\n";
  }
  {
    std::ofstream out(fixture, std::ios::app);
    out << "sensor_types:\n  imu948:\n    notify_handle: 8\n    cccd_handle: 9\n    write_handle: 6\ndevices:\n  - mac: \"AA:BB:CC:DD:EE:FF\"\n    type: imu948\n";
  }
  const auto loaded = iot::AppConfig::load(fixture);
  std::filesystem::remove(fixture);
  expect(loaded.collection_schedule.enabled && loaded.collection_schedule.start_time == "14:00" &&
         loaded.collection_schedule.stop_time == "17:00", "schedule config parsing failed");
}

void test_websocket_rejection() {
  for (const auto* json : {
      R"({"type":"subscribe","mac":"AA:BB:CC:DD:EE:FF","sample_hz":-1})",
      R"({"type":"subscribe","mac":"AA:BB:CC:DD:EE:FF","sample_hz":1.5})",
      R"({"type":"subscribe","mac":"AA:BB:CC:DD:EE:FF","sample_hz":"10"})",
      R"({"type":"subscribe","mac":"","mac":"AA:BB:CC:DD:EE:FF"})",
      R"({"type":"subscribe","mac":"",})",
      R"({"type":"subscribe","mac":"", "unknown":0})"}) {
    bool rejected = false;
    try { (void)iot::protocol::parse_subscription(json); }
    catch (const std::exception&) { rejected = true; }
    expect(rejected, "invalid subscription accepted");
  }
  const auto valid = iot::protocol::parse_subscription(
      " { \"mac\" : \"aa:bb:cc:dd:ee:ff\", \"type\" : \"subscribe\" } ");
  expect(valid.mac == "AA:BB:CC:DD:EE:FF" && valid.sample_hz == 0,
         "subscription defaults or normalization failed");
  for (const auto& bytes : std::vector<iot::Bytes>{
      {0x81, 0x00},                     // Unmasked client frame.
      {0x09, 0x80},                     // Fragmented ping.
      {0x81, 0xfe, 0x10, 0x01},         // Oversized command.
      {0x80, 0x80, 0, 0, 0, 0},         // Continuation without start.
      {0xc1, 0x80},                     // Unsupported extension bits.
      {0x81, 0xfe, 0, 1}}) {            // Noncanonical extended length.
    iot::protocol::WebSocketDecoder decoder;
    bool rejected = false;
    try { decoder.append(bytes.data(), bytes.size(), [](const auto&) {}); }
    catch (const std::exception&) { rejected = true; }
    expect(rejected, "invalid WebSocket frame accepted");
  }
}

void test_decoder_resynchronization() {
  auto frame = iot::protocol::create_command("FF:FF:FF:FF:FF:FF", 0x22, 7, {0, 0, 0, 1, 0, 0, 0, 0});
  int count = 0, errors = 0;
  iot::protocol::StreamDecoder decoder({[&](const auto&) { ++count; }, {}, {}, [&](const auto&) { ++errors; }});
  iot::Bytes input{0xde, 0xad}; input.insert(input.end(), frame.begin(), frame.end());
  decoder.append(input);
  expect(count == 1 && errors == 1, "stream did not resynchronize");
  iot::Bytes noise(4096, 0xdd);
  noise.push_back(frame.front());
  decoder.append(noise);
  decoder.append(frame.data() + 1, frame.size() - 1);
  expect(count == 2 && errors == 2, "noise recovery must preserve split headers without flooding logs");
}

void test_scan_frame() {
  iot::Bytes frame{0xad, 0xba, 0x00, 0x01, 0x01, 0x00, 0x34,
                   0xd6, 0xea, 0xa6, 0xe2, 0xb0, 0x70, 0, 0, 0, 1, 0, 1,
                   0xce, 0x43, 0x75, 0x25, 0x96, 0xbf, static_cast<std::uint8_t>(-42)};
  frame.resize(57, 0);  // one type-01 device block ends at byte 56
  const auto checksum = iot::protocol::xor_checksum(frame.data() + 5, frame.size() - 5);
  frame.push_back(checksum); frame.push_back(checksum);
  int reports = 0;
  iot::ScanEvent event;
  iot::protocol::StreamDecoder decoder({{}, [&](const auto& found) { event = found; ++reports; },
                                        [](const auto& mac) { return mac == "CE:43:75:25:96:BF"; }, {}});
  decoder.append(frame);
  expect(reports == 1 && event.rssi == -42, "scan report decode failed");
  // The gateway's MAC/RSSI-only mode has seven bytes per device.
  frame.resize(26);
  frame[4] = 3;
  frame[6] = 21;
  const auto short_checksum = iot::protocol::xor_checksum(frame.data() + 5, frame.size() - 5);
  frame.push_back(short_checksum); frame.push_back(short_checksum);
  decoder.append(frame);
  expect(reports == 2 && event.mac == "CE:43:75:25:96:BF", "compact scan report decode failed");
}

void test_imu948_parser() {
  // tag, flags(accel+gyro), timestamp, accel xyz, gyro xyz (all little endian)
  iot::Bytes payload{0x11, 0x05, 0x00, 0x78, 0x56, 0x34, 0x12,
                     0x01, 0x00, 0xfe, 0xff, 0x64, 0x00,
                     0x0a, 0x00, 0xec, 0xff, 0x1e, 0x00};
  const auto parsed = iot::protocol::Imu948Parser{}.parse("AA:BB:CC:DD:EE:FF", payload);
  expect(parsed.has_value(), "IMU948 sample was rejected");
  const auto& sample = std::get<iot::SensorSample>(*parsed);
  expect(sample.sensor_uptime_ms == 0x12345678, "IMU timestamp endian mismatch");
  expect(sample.accel && std::abs(sample.accel->x - 0.00478515625) < 1e-10, "IMU accel scale mismatch");
  expect(sample.gyro && sample.gyro->y < 0, "IMU signed gyro mismatch");
  payload.pop_back();
  expect(!iot::protocol::Imu948Parser{}.parse("AA:BB:CC:DD:EE:FF", payload), "truncated IMU payload accepted");
}

void test_md9930_parser() {
  iot::Bytes payload{0x01, 0x00, 0xfe, 0xff, 0x64, 0x00,
                     0x00, 0x0a, 0xff, 0xec, 0x00, 0x1e,
                     0x12, 0x34, 0x00, 0x01, 0x86, 0xa0};
  const auto parsed = iot::protocol::Md9930Parser{}.parse("54:6C:0E:B9:1A:4C", payload);
  expect(parsed.has_value(), "MD9930 sample was rejected");
  const auto& sample = std::get<iot::SensorSample>(*parsed);
  expect(sample.counter == 0x1234 && sample.ticks == 100000, "MD9930 endian mismatch");
  expect(sample.sensor_uptime_ms == 1000, "MD9930 ticks conversion mismatch");
  expect(sample.gyro && std::abs(sample.gyro->x - 0.7) < 1e-10, "MD9930 gyro scale mismatch");
  // Consecutive real V20 packets: a little-endian interpretation jumps by 256.
  const auto first = iot::protocol::Md9930Parser{}.parse("54:6C:0E:B8:37:E4",
      {0x3a,0,0xf8,0xff,0xfa,0xf7,0,0x1c,0xff,0xe0,0xff,0xf3,0x90,0xf5,0xa9,0x0e,0x8b,0xd8});
  const auto next = iot::protocol::Md9930Parser{}.parse("54:6C:0E:B8:37:E4",
      {0x27,0,0xfd,0,0x0f,0xf8,0,0x16,0xff,0xda,0xff,0xf3,0x90,0xf6,0xa9,0x0e,0x9f,0x96});
  expect(first && next, "captured MD9930 packets rejected");
  const auto& a = std::get<iot::SensorSample>(*first);
  const auto& b = std::get<iot::SensorSample>(*next);
  expect(*b.counter == *a.counter + 1 && *b.ticks - *a.ticks == 5054,
         "captured MD9930 counter/timestamp byte order mismatch");
}

class FakeBackend final : public iot::services::InferenceBackend {
 public: int infer(const float* input, std::size_t length) override { expect(length == 9, "inference layout size"); expect(input[3] == 2, "input is not channel-major"); return 4; }
};

void test_inference_window() {
  iot::InferenceConfig config; config.enabled = true; config.window_size = 3; config.stride = 2;
  iot::services::InferenceEngine engine(config, std::make_unique<FakeBackend>());
  iot::SensorSample sample; sample.mac = "AA";
  sample.accel = iot::Vec3{1, 2, 3}; expect(!engine.process(sample), "inference ran too early");
  sample.accel = iot::Vec3{2, 2, 3}; expect(!engine.process(sample), "inference ran too early");
  sample.accel = iot::Vec3{3, 2, 3}; expect(!engine.process(sample), "stride was ignored");
  sample.accel = iot::Vec3{4, 2, 3}; expect(engine.process(sample) == "walking", "inference class mapping failed");
}

void test_bounded_queue() {
  using iot::concurrency::BoundedQueue;
  using iot::concurrency::OverflowPolicy;
  using iot::concurrency::PushResult;

  BoundedQueue<int> fifo(2);
  expect(fifo.try_push(1) == PushResult::pushed, "queue rejected first item");
  expect(fifo.try_push(2) == PushResult::pushed, "queue rejected second item");
  expect(fifo.try_push(3) == PushResult::rejected, "full queue did not reject newest item");
  fifo.close();
  expect(fifo.wait_pop() == 1, "queue FIFO order is wrong");
  expect(fifo.wait_pop() == 2, "queue did not drain after close");
  expect(!fifo.wait_pop(), "closed empty queue did not terminate consumer");
  expect(fifo.try_push(4) == PushResult::closed, "closed queue accepted an item");

  BoundedQueue<int> realtime(2, OverflowPolicy::drop_oldest);
  expect(realtime.try_push(1) == PushResult::pushed, "realtime queue rejected first item");
  expect(realtime.try_push(2) == PushResult::pushed, "realtime queue rejected second item");
  expect(realtime.try_push(3) == PushResult::dropped_oldest,
         "realtime queue did not drop its oldest item");
  realtime.close();
  expect(realtime.wait_pop() == 2 && realtime.wait_pop() == 3,
         "realtime queue retained the wrong items");
  expect(realtime.dropped() == 1, "queue drop counter is wrong");
}

void test_inference_worker() {
  iot::InferenceConfig config;
  config.enabled = true;
  config.window_size = 3;
  config.stride = 2;

  std::mutex mutex;
  std::condition_variable ready;
  bool predicted = false;
  std::string predicted_mac;
  std::string predicted_action;
  iot::services::InferenceWorker worker(
      config, std::make_unique<FakeBackend>(),
      [&](const std::string& mac, const std::string& action) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          predicted = true;
          predicted_mac = mac;
          predicted_action = action;
        }
        ready.notify_one();
      },
      8);
  worker.start();

  iot::SensorSample sample;
  sample.mac = "AA";
  for (int value = 1; value <= 4; ++value) {
    sample.accel = iot::Vec3{static_cast<double>(value), 2, 3};
    expect(worker.enqueue(sample), "inference worker rejected a sample");
  }

  std::unique_lock<std::mutex> lock(mutex);
  const bool completed = ready.wait_for(lock, std::chrono::seconds(1), [&] { return predicted; });
  lock.unlock();
  worker.stop();
  expect(completed, "inference worker did not publish a prediction");
  expect(predicted_mac == "AA" && predicted_action == "walking",
         "inference worker published the wrong prediction");
}

}  // namespace

int main() {
  try {
    test_config();
    test_collection_schedule();
    test_websocket_rejection();
    test_gateway_command_and_stream_decoder();
    test_decoder_resynchronization();
    test_scan_frame();
    test_imu948_parser();
    test_md9930_parser();
    test_inference_window();
    test_bounded_queue();
    test_inference_worker();
    std::cout << "All protocol tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "TEST FAILED: " << error.what() << '\n';
    return 1;
  }
}
