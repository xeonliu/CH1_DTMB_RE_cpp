#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace lme2510 {

/// Local wall-clock fields; used instead of platform-local time functions so
/// callers stay portable (Windows XP/MinGW has no localtime_s, for example).
struct LocalDateTime {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;
};

/// Global stop flag installed by installStopHandlers().  Main loops check it
/// periodically; a signal or Windows console event sets it.
std::atomic<bool>& stopRequested();

/// Thread-safe local wall-clock read, implemented per platform.
LocalDateTime localDateTime();

/// Parses a numeric IPv4 host such as "127.0.0.1" into network byte order.
/// Windows XP has no inet_pton, so the platform implementation picks the
/// matching API (inet_addr there, inet_pton elsewhere).
bool parseIPv4Host(const std::string& host, std::uint32_t& networkOrder);

/// Installs SIGINT/SIGTERM handlers on POSIX and SetConsoleCtrlHandler on
/// Windows.  Must be called once before starting the stream loop.
void installStopHandlers();

/// WSAStartup on Windows, no-op elsewhere.  Returns true on success.
bool platformInitialize();

/// WSACleanup on Windows, no-op elsewhere.
void platformCleanup();

void sleepMilliseconds(int ms);
void sleepSeconds(double seconds);

}  // namespace lme2510
