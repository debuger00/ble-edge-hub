#include "iot/services/health_monitor.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace std::chrono;
using namespace iot;
using namespace iot::services;
namespace {
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void test_tracker() {
  AppConfig config;
  config.alerts.enabled = true;
  config.alerts.timeout_seconds = 300;
  config.devices = {{"AA:BB:CC:DD:EE:FF", "imu948", "001"},
                    {"AA:BB:CC:DD:EE:00", "imu948", "002"}};
  const auto wall = Clock::time_point{} + hours(6); // Beijing 14:00
  const auto steady = HealthTracker::Steady::time_point{};
  HealthTracker tracker(config);
  expect(tracker.due(wall, steady).empty(), "startup grace period");
  expect(tracker.due(wall + seconds(299), steady + seconds(299)).empty(), "must not alert early");
  tracker.sample(config.devices[1].mac, wall + seconds(299), steady + seconds(299));
  auto alerts = tracker.due(wall + seconds(300), steady + seconds(300));
  expect(alerts.size() == 1 && alerts[0].id == "001" && !alerts[0].had_sample,
         "never-connected device must alert, healthy device must not");
  expect(alerts[0].missing_since == wall && alerts[0].missing_seconds == 300, "incident timestamp");
  expect(tracker.due(wall + seconds(359), steady + seconds(359)).empty(), "retry must be throttled");
  alerts = tracker.due(wall + seconds(360), steady + seconds(360));
  expect(alerts.size() == 1, "failed delivery must retry");
  tracker.delivered(alerts);
  expect(tracker.due(wall + seconds(500), steady + seconds(500)).empty(), "no duplicate on same incident");
  tracker.sample(config.devices[0].mac, wall + seconds(501), steady + seconds(501));
  tracker.sample(config.devices[1].mac, wall + seconds(800), steady + seconds(800));
  alerts = tracker.due(wall + seconds(801), steady + seconds(801));
  expect(alerts.size() == 1 && alerts[0].had_sample, "recovery must rearm alerts");
  const auto body = alert_body(alerts, 300);
  expect(body.find("001") != std::string::npos && body.find(config.devices[0].mac) != std::string::npos &&
         body.find("1970-01-01 14:08:21") != std::string::npos && body.find("UTC+8") != std::string::npos,
         "email must contain device mapping and Beijing incident timestamp");
  tracker.sample(config.devices[0].mac, wall + seconds(802), steady + seconds(802));
  tracker.delivered(alerts); // SMTP completed after recovery; must not silence a new incident.
  tracker.sample(config.devices[1].mac, wall + seconds(1101), steady + seconds(1101));
  expect(tracker.due(wall + seconds(1102), steady + seconds(1102)).size() == 1, "stale delivery acknowledgement");

  auto disabled_config = config;
  disabled_config.devices[0].enabled = false;
  HealthTracker disabled_tracker(disabled_config);
  disabled_tracker.due(wall, steady);
  const auto disabled_alerts = disabled_tracker.due(wall + seconds(301), steady + seconds(301));
  expect(disabled_alerts.size() == 1 && disabled_alerts[0].id == "002", "disabled device must not alert");

  HealthTracker clock_tracker(config);
  clock_tracker.due(wall, steady);
  expect(clock_tracker.due(wall + hours(10), steady + seconds(299)).empty(), "wall jump must not accelerate timeout");
  expect(clock_tracker.due(wall - hours(1), steady + seconds(300)).size() == 2, "wall rollback must not delay timeout");

  config.collection_schedule.enabled = true;
  HealthTracker scheduled(config);
  expect(scheduled.due(wall - hours(1), steady).empty(), "outside schedule must not monitor");
  expect(scheduled.due(wall, steady + hours(1)).empty(), "schedule starts fresh grace period");
  expect(scheduled.due(wall + minutes(5), steady + hours(1) + minutes(5)).size() == 2, "scheduled missing devices");
  expect(scheduled.due(wall + hours(3), steady + hours(4)).empty(), "planned stop must reset incidents");
  expect(scheduled.due(wall + hours(24), steady + hours(25)).empty(), "next day fresh grace");
  expect(scheduled.due(wall + hours(24) + minutes(5), steady + hours(25) + minutes(5)).size() == 2,
         "next day missing devices must alert again");
  config.alerts.enabled = false;
  expect(scheduled.due(wall, steady + hours(50)).empty(), "master switch");
}

void test_config_validation() {
  auto config = AppConfig::load(std::filesystem::path(IOT_TEST_SOURCE_DIR) / "config/config.yaml");
  expect(config.devices[0].id == "001" && config.devices[4].id == "005", "load id mapping");
  expect(!config.alerts.enabled && config.alerts.timeout_seconds == 300, "safe alert defaults");
  config.alerts.enabled = true;
  config.alerts.smtp_password = "test-code";
  config.validate();
  auto rejects = [](const AppConfig& cfg) {
    try { cfg.validate(); } catch (const std::runtime_error&) { return true; }
    return false;
  };
  auto bad = config;
  bad = config;
  bad.devices[1].id = bad.devices[0].id;
  expect(rejects(bad), "duplicate IDs");
  bad = config; bad.devices[0].id.clear(); expect(rejects(bad), "missing IDs");
  bad = config; bad.alerts.timeout_seconds = 0; expect(rejects(bad), "invalid timeout");
  bad = config; bad.alerts.to_email = "a@b\nBcc: x@y"; expect(rejects(bad), "header injection");
  bad = config; bad.alerts.smtp_password.clear(); expect(rejects(bad), "missing SMTP credentials");

  const auto path = std::filesystem::temp_directory_path() / ("iot-alert-config-" + std::to_string(getpid()) + ".yaml");
  {
    std::ifstream input(std::filesystem::path(IOT_TEST_SOURCE_DIR) / "config/config.yaml");
    std::ofstream output(path);
    output << input.rdbuf() << "\nalerts:\n  smtp_password: 'code#with:hash' # comment\n";
  }
  const auto parsed = AppConfig::load(path);
  std::filesystem::remove(path);
  expect(parsed.alerts.smtp_password == "code#with:hash", "quoted SMTP code parsing");
}
}
int main() {
  try { test_tracker(); test_config_validation(); std::cout << "Health monitor tests passed\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
