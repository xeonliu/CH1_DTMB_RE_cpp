#pragma once

#include <atomic>

namespace lme2510 {

/// Global stop flag installed by installStopHandlers().  Main loops check it
/// periodically; a signal or Windows console event sets it.
std::atomic<bool>& stopRequested();

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
