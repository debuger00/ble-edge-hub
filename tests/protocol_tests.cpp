#include "iot/config.hpp"
#include "iot/concurrency/bounded_queue.hpp"
#include "iot/protocol/gateway_codec.hpp"
#include "iot/protocol/sensor_parser.hpp"
#include "iot/services/inference_engine.hpp"
#include "iot/services/inference_worker.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
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
  expect(config.tcp.port == 7628, "TCP config was not loaded");
  expect(config.queues.database_capacity == 30000, "queue config was not loaded");
  expect(config.profile_for_mac("54:6c:0e:b9:1a:4c").name == "md9930", "MAC normalization failed");
  bool rejected = false;
  try { (void)config.profile_for_mac("00:00:00:00:00:00"); } catch (const std::out_of_range&) { rejected = true; }
  expect(rejected, "unknown MAC unexpectedly got a fallback profile");
}

void test_decoder_resynchronization() {
  auto frame = iot::protocol::create_command("FF:FF:FF:FF:FF:FF", 0x22, 7, {0, 0, 0, 1, 0, 0, 0, 0});
  int count = 0, errors = 0;
  iot::protocol::StreamDecoder decoder({[&](const auto&) { ++count; }, {}, {}, [&](const auto&) { ++errors; }});
  iot::Bytes input{0xde, 0xad}; input.insert(input.end(), frame.begin(), frame.end());
  decoder.append(input);
  expect(count == 1 && errors == 2, "stream did not resynchronize");
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
                     0x34, 0x12, 0xa0, 0x86, 0x01, 0x00};
  const auto parsed = iot::protocol::Md9930Parser{}.parse("54:6C:0E:B9:1A:4C", payload);
  expect(parsed.has_value(), "MD9930 sample was rejected");
  const auto& sample = std::get<iot::SensorSample>(*parsed);
  expect(sample.counter == 0x1234 && sample.ticks == 100000, "MD9930 endian mismatch");
  expect(sample.sensor_uptime_ms == 1000, "MD9930 ticks conversion mismatch");
  expect(sample.gyro && std::abs(sample.gyro->x - 0.7) < 1e-10, "MD9930 gyro scale mismatch");
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
