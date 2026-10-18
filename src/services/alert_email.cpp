#include "iot/services/health_monitor.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <sstream>

#include "iot/json.hpp"

extern char** environ;

namespace iot::services {
namespace {
// Python's standard library supplies SMTP, MIME UTF-8 and certificate verification.
// No shell, external packages, temporary credential files or plaintext error dumps.
constexpr char sender[] = R"PY(
import json, smtplib, ssl, sys
from email.message import EmailMessage
from email.utils import formatdate, make_msgid
try:
    c = json.load(sys.stdin)
    msg = EmailMessage()
    msg['From'] = c['from_email']
    msg['To'] = c['to_email']
    msg['Subject'] = '传感器采集超时报警 / Sensor collection timeout'
    msg['Date'] = formatdate(localtime=False)
    msg['Message-ID'] = make_msgid()
    msg.set_content(c['body'])
    context = ssl.create_default_context()
    if c['security'] == 'ssl':
        smtp = smtplib.SMTP_SSL(c['host'], c['port'], timeout=c['timeout'], context=context)
    else:
        smtp = smtplib.SMTP(c['host'], c['port'], timeout=c['timeout'])
    try:
        if c['security'] == 'starttls':
            smtp.starttls(context=context)
        if c['username']:
            smtp.login(c['username'], c['password'])
        smtp.send_message(msg)
    finally:
        smtp.close()
except Exception:
    sys.exit(1)
)PY";
}  // namespace

bool send_alert_email(const AlertConfig& config, const std::vector<DeviceAlert>& alerts,
                      const std::atomic<bool>& stopping) {
  if (stopping || alerts.empty()) return false;
  std::ostringstream json;
  json << "{\"host\":\"" << json_escape(config.smtp_host) << "\",\"port\":" << config.smtp_port
       << ",\"security\":\"" << json_escape(config.smtp_security)
       << "\",\"username\":\"" << json_escape(config.smtp_username)
       << "\",\"password\":\"" << json_escape(config.smtp_password)
       << "\",\"from_email\":\"" << json_escape(config.from_email)
       << "\",\"to_email\":\"" << json_escape(config.to_email)
       << "\",\"timeout\":" << config.smtp_timeout_seconds
       << ",\"body\":\"" << json_escape(alert_body(alerts, config.timeout_seconds)) << "\"}";
  const auto payload = json.str();
  int input[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input) != 0) return false;
  // Some daemon launchers close stdin/stdout/stderr; keep child input above fd 2.
  for (auto& fd : input) {
    if (fd > STDERR_FILENO) continue;
    const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (replacement < 0) { close(input[0]); close(input[1]); return false; }
    close(fd);
    fd = replacement;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, input[1], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&actions, input[0]);
  posix_spawn_file_actions_addclose(&actions, input[1]);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  char* argv[] = {const_cast<char*>(IOT_PYTHON_EXECUTABLE), const_cast<char*>("-I"),
                  const_cast<char*>("-c"), const_cast<char*>(sender), nullptr};
  pid_t child{};
  const int spawned = posix_spawn(&child, IOT_PYTHON_EXECUTABLE, &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(input[1]);
  if (spawned != 0) { close(input[0]); return false; }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.smtp_timeout_seconds);
  std::size_t offset = 0;
  bool write_failed = false;
  while (offset < payload.size() && !stopping && std::chrono::steady_clock::now() < deadline) {
    const auto count = send(input[0], payload.data() + offset, payload.size() - offset,
                            MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) offset += static_cast<std::size_t>(count);
    else if (count < 0 && errno == EINTR) continue;
    else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd descriptor{input[0], POLLOUT, 0};
      poll(&descriptor, 1, 50);
    } else { write_failed = true; break; }
  }
  shutdown(input[0], SHUT_WR);
  close(input[0]);
  int status{};
  while (!write_failed && offset == payload.size() && !stopping &&
         std::chrono::steady_clock::now() < deadline) {
    const auto result = waitpid(child, &status, WNOHANG);
    if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (result < 0 && errno != EINTR) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return false;
}
}  // namespace iot::services
