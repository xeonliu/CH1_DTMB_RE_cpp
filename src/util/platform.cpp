#include "lme2510/util/platform.hpp"

#include <chrono>
#include <csignal>
#include <ctime>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

namespace lme2510 {
namespace {

std::atomic<bool> g_stopRequested{false};

void handleSignal(int) {
  g_stopRequested.store(true);
}

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD /*ctrlType*/) {
  g_stopRequested.store(true);
  return TRUE;
}
#endif

}  // namespace

std::atomic<bool>& stopRequested() {
  return g_stopRequested;
}

LocalDateTime localDateTime() {
  LocalDateTime result;
#ifdef _WIN32
  SYSTEMTIME st{};
  GetLocalTime(&st);
  result.year = st.wYear;
  result.month = st.wMonth;
  result.day = st.wDay;
  result.hour = st.wHour;
  result.minute = st.wMinute;
  result.second = st.wSecond;
  result.millisecond = st.wMilliseconds;
#else
  using Clock = std::chrono::system_clock;
  const auto now = Clock::now();
  const auto time = Clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count() %
                  1000;
  std::tm local{};
  localtime_r(&time, &local);
  result.year = local.tm_year + 1900;
  result.month = local.tm_mon + 1;
  result.day = local.tm_mday;
  result.hour = local.tm_hour;
  result.minute = local.tm_min;
  result.second = local.tm_sec;
  result.millisecond = static_cast<int>(ms);
#endif
  return result;
}

bool parseIPv4Host(const std::string& host, std::uint32_t& networkOrder) {
#ifdef _WIN32
  // inet_pton/InetPton is Vista+; inet_addr handles the same numeric IPv4
  // input and is available since Winsock 1.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // inet_addr: deprecated but XP-compatible
#endif
  const unsigned long value = ::inet_addr(host.c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  // inet_addr reports an error as INADDR_NONE; broadcast 255.255.255.255 is
  // the one legitimate address that has that value.
  if (value == INADDR_NONE && host != "255.255.255.255") {
    return false;
  }
  networkOrder = static_cast<std::uint32_t>(value);
  return true;
#else
  in_addr address{};
  if (::inet_pton(AF_INET, host.c_str(), &address) != 1) {
    return false;
  }
  networkOrder = static_cast<std::uint32_t>(address.s_addr);
  return true;
#endif
}

void installStopHandlers() {
  stopRequested().store(false);
#ifdef _WIN32
  SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
#endif
}

bool platformInitialize() {
#ifdef _WIN32
  WSADATA data{};
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  return true;
#endif
}

void platformCleanup() {
#ifdef _WIN32
  WSACleanup();
#endif
}

void sleepMilliseconds(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void sleepSeconds(double seconds) {
  const auto duration =
      std::chrono::duration<double>(seconds < 0.0 ? 0.0 : seconds);
  std::this_thread::sleep_for(duration);
}

}  // namespace lme2510
