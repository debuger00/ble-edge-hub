#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace iot {

enum class LogLevel { debug, info, warning, error };

inline void log(LogLevel level, const std::string& message) {
  static std::mutex mutex;
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &time);  // Unix 时间戳(秒数)转换成本地时区的日历时间,即把 time_t 转换成 std::tm
                            // 结构体(年/月/日/时/分/秒)。
#else
  localtime_r(&time, &tm);
#endif
  const char* name = level == LogLevel::debug     ? "DEBUG"
                     : level == LogLevel::info    ? "INFO"
                     : level == LogLevel::warning ? "WARN"
                                                  : "ERROR";
  std::lock_guard<std::mutex> lock(mutex);  //互斥锁
  std::clog << std::put_time(&tm, "%F %T") << " [" << name << "] " << message << '\n';
}

}  // namespace iot
