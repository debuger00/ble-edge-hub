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
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  const char* name = level == LogLevel::debug ? "DEBUG" : level == LogLevel::info ? "INFO" :
                     level == LogLevel::warning ? "WARN" : "ERROR";
  std::lock_guard<std::mutex> lock(mutex);
  std::clog << std::put_time(&tm, "%F %T") << " [" << name << "] " << message << '\n';
}

}  // namespace iot

