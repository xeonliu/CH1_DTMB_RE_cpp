#include "lme2510/util/platform.hpp"

#include <chrono>
#include <csignal>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
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
