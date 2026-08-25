#pragma once

#include "iot/domain.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace iot::services {

class DatabaseWriter {
 public:
  DatabaseWriter(std::filesystem::path base_dir, int retention_days, std::size_t queue_capacity = 10000);
  ~DatabaseWriter();
  DatabaseWriter(const DatabaseWriter&) = delete;
  DatabaseWriter& operator=(const DatabaseWriter&) = delete;

  bool enqueue(SensorSample sample);
  void stop();

 private:
  void run();
  void cleanup_old_databases();

  std::filesystem::path base_dir_;
  int retention_days_;
  std::size_t queue_capacity_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<SensorSample> queue_;
  bool stopping_{false};
  std::thread worker_;
};

}  // namespace iot::services

