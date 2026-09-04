#include "iot/services/database_writer.hpp"

#include "iot/log.hpp"

#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace iot::services {
namespace {

struct sqlite3;
struct sqlite3_stmt;
constexpr int kSqliteOk = 0;
constexpr int kSqliteDone = 101;
constexpr int kOpenReadWrite = 0x00000002;
constexpr int kOpenCreate = 0x00000004;
constexpr int kOpenFullMutex = 0x00010000;

class SqliteApi {
 public:
  using Destructor = void (*)(void*);
  SqliteApi() {
    library_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library_) library_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    if (!library_) throw std::runtime_error("SQLite runtime library is unavailable");
    load(open_v2, "sqlite3_open_v2"); load(close, "sqlite3_close"); load(exec, "sqlite3_exec");
    load(prepare_v2, "sqlite3_prepare_v2"); load(finalize, "sqlite3_finalize");
    load(bind_double, "sqlite3_bind_double"); load(bind_int64, "sqlite3_bind_int64");
    load(bind_text, "sqlite3_bind_text"); load(bind_null, "sqlite3_bind_null");
    load(step, "sqlite3_step"); load(reset, "sqlite3_reset"); load(clear_bindings, "sqlite3_clear_bindings");
    load(errmsg, "sqlite3_errmsg");
  }
  ~SqliteApi() { if (library_) dlclose(library_); }
  int (*open_v2)(const char*, sqlite3**, int, const char*);
  int (*close)(sqlite3*);
  int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
  int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
  int (*finalize)(sqlite3_stmt*);
  int (*bind_double)(sqlite3_stmt*, int, double);
  int (*bind_int64)(sqlite3_stmt*, int, long long);
  int (*bind_text)(sqlite3_stmt*, int, const char*, int, Destructor);
  int (*bind_null)(sqlite3_stmt*, int);
  int (*step)(sqlite3_stmt*);
  int (*reset)(sqlite3_stmt*);
  int (*clear_bindings)(sqlite3_stmt*);
  const char* (*errmsg)(sqlite3*);
 private:
  template <typename T> void load(T& target, const char* name) {
    target = reinterpret_cast<T>(dlsym(library_, name));
    if (!target) throw std::runtime_error(std::string("missing SQLite symbol: ") + name);
  }
  void* library_{};
};

std::string day_string(Clock::time_point time, const char* format) {
  const auto raw = Clock::to_time_t(time);
  std::tm tm{};
  localtime_r(&raw, &tm);
  std::ostringstream out;
  out << std::put_time(&tm, format);
  return out.str();
}

void exec_or_throw(SqliteApi& api, sqlite3* db, const char* sql) {
  if (api.exec(db, sql, nullptr, nullptr, nullptr) != kSqliteOk)
    throw std::runtime_error(api.errmsg(db));
}

void bind_optional(SqliteApi& api, sqlite3_stmt* statement, int index, const std::optional<Vec3>& vec, int axis) {
  if (!vec) { api.bind_null(statement, index); return; }
  const double value = axis == 0 ? vec->x : axis == 1 ? vec->y : vec->z;
  api.bind_double(statement, index, value);
}

class Connection {
 public:
  Connection(SqliteApi& api, const std::filesystem::path& path) : api_(api) {
    std::filesystem::create_directories(path.parent_path());
    if (api_.open_v2(path.c_str(), &db_, kOpenReadWrite | kOpenCreate | kOpenFullMutex, nullptr) != kSqliteOk)
      throw std::runtime_error("cannot open SQLite database: " + path.string());
    exec_or_throw(api_, db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    exec_or_throw(api_, db_,
      "CREATE TABLE IF NOT EXISTS sensor_data("
      "ts REAL NOT NULL,date TEXT NOT NULL,mac TEXT NOT NULL,uptime_ms INTEGER,"
      "accel_x REAL,accel_y REAL,accel_z REAL,gyro_x REAL,gyro_y REAL,gyro_z REAL,"
      "mag_x REAL,mag_y REAL,mag_z REAL);"
      "CREATE INDEX IF NOT EXISTS idx_sensor_mac_ts ON sensor_data(mac,ts);");
    const char* sql = "INSERT INTO sensor_data VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (api_.prepare_v2(db_, sql, -1, &statement_, nullptr) != kSqliteOk) throw std::runtime_error(api_.errmsg(db_));
  }
  ~Connection() { if (statement_) api_.finalize(statement_); if (db_) api_.close(db_); }
  void insert(const SensorSample& sample) {
    const auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sample.received_at.time_since_epoch()).count();
    const auto date = day_string(sample.received_at, "%F %T");
    api_.bind_double(statement_, 1, ts_ms / 1000.0);
    api_.bind_text(statement_, 2, date.c_str(), -1, reinterpret_cast<SqliteApi::Destructor>(-1));
    api_.bind_text(statement_, 3, sample.mac.c_str(), -1, reinterpret_cast<SqliteApi::Destructor>(-1));
    if (sample.sensor_uptime_ms) api_.bind_int64(statement_, 4, static_cast<long long>(*sample.sensor_uptime_ms)); else api_.bind_null(statement_, 4);
    for (int axis = 0; axis < 3; ++axis) bind_optional(api_, statement_, 5 + axis, sample.accel, axis);
    for (int axis = 0; axis < 3; ++axis) bind_optional(api_, statement_, 8 + axis, sample.gyro, axis);
    for (int axis = 0; axis < 3; ++axis) bind_optional(api_, statement_, 11 + axis, sample.mag, axis);
    if (api_.step(statement_) != kSqliteDone) throw std::runtime_error(api_.errmsg(db_));
    api_.reset(statement_); api_.clear_bindings(statement_);
  }
  void begin() { exec_or_throw(api_, db_, "BEGIN IMMEDIATE"); }
  void commit() { exec_or_throw(api_, db_, "COMMIT"); }
 private:
  SqliteApi& api_;
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

}  // namespace

DatabaseWriter::DatabaseWriter(std::filesystem::path base_dir, int retention_days, std::size_t queue_capacity)
    : base_dir_(std::move(base_dir)), retention_days_(retention_days), queue_capacity_(queue_capacity) {
  cleanup_old_databases();
  worker_ = std::thread(&DatabaseWriter::run, this);
}

DatabaseWriter::~DatabaseWriter() { stop(); }

bool DatabaseWriter::enqueue(SensorSample sample) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || queue_.size() >= queue_capacity_) return false;
  queue_.push_back(std::move(sample));
  ready_.notify_one();
  return true;
}

std::size_t DatabaseWriter::queued() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void DatabaseWriter::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  ready_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void DatabaseWriter::run() {
  try {
    SqliteApi api;
    std::string open_day;
    std::unique_ptr<Connection> connection;
    while (true) {
      std::vector<SensorSample> batch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, std::chrono::seconds(2), [&] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) break;
        const auto count = std::min<std::size_t>(100, queue_.size());
        for (std::size_t i = 0; i < count; ++i) { batch.push_back(std::move(queue_.front())); queue_.pop_front(); }
      }
      for (std::size_t begin = 0; begin < batch.size();) {
        const auto day = day_string(batch[begin].received_at, "%Y_%m_%d");
        if (day != open_day) {
          connection = std::make_unique<Connection>(api, base_dir_ / day / "sensor.db");
          open_day = day;
        }
        connection->begin();
        std::size_t end = begin;
        while (end < batch.size() && day_string(batch[end].received_at, "%Y_%m_%d") == day) connection->insert(batch[end++]);
        connection->commit();
        begin = end;
      }
    }
  } catch (const std::exception& error) {
    log(LogLevel::error, std::string("database writer stopped: ") + error.what());
  }
}

void DatabaseWriter::cleanup_old_databases() {
  if (retention_days_ <= 0 || !std::filesystem::exists(base_dir_)) return;
  const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * retention_days_);
  for (const auto& entry : std::filesystem::directory_iterator(base_dir_)) {
    std::error_code error;
    if (entry.is_directory(error) && entry.last_write_time(error) < cutoff) {
      std::filesystem::remove_all(entry.path(), error);
      if (error) log(LogLevel::warning, "cannot remove expired data directory: " + entry.path().string());
    }
  }
}

}  // namespace iot::services
