#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "lme2510/frontend/receiver.hpp"
#include "lme2510/stream/stream_session.hpp"
#include "lme2510/util/arg_parser.hpp"
#include "lme2510/util/logger.hpp"
#include "lme2510/util/platform.hpp"

namespace {

std::string timestampForFileName() {
  const lme2510::LocalDateTime now = lme2510::localDateTime();
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
                now.year, now.month, now.day, now.hour, now.minute,
                now.second);
  return buffer;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace lme2510;

  Options options;
  std::string parseError;
  bool showHelp = false;
  if (!parseArgs(argc, argv, options, parseError, showHelp)) {
    std::cerr << "error: " << parseError << "\n\n";
    printHelp();
    return 2;
  }
  if (showHelp) {
    printHelp();
    return 0;
  }

  installStopHandlers();
  if (!platformInitialize()) {
    std::cerr << "error: platform socket initialization failed\n";
    return 1;
  }

  struct SocketGuard {
    ~SocketGuard() { platformCleanup(); }
  } socketGuard;

  try {
    const std::string stamp = timestampForFileName();
    std::error_code ignored;
    std::filesystem::create_directories("logs", ignored);

    if (options.statusLogPath.empty()) {
      options.statusLogPath = "logs/stream-" + stamp + ".log";
    }
    if (options.regLogPath.empty()) {
      options.regLogPath = "logs/regs-" + stamp + ".log";
    }

    std::cout << "status log : " << options.statusLogPath << '\n';
    std::cout << "register log: " << options.regLogPath << '\n';

    RegLogger regLogger(options.regLogPath, options.usbTrace);
    std::ofstream statusLog(options.statusLogPath,
                            std::ios::out | std::ios::app);
    if (!statusLog) {
      throw std::runtime_error("cannot open status log: " +
                               options.statusLogPath);
    }

    ReceiverOptions receiverOptions;
    receiverOptions.frequencyMhz = options.frequencyMhz;
    receiverOptions.pidList = options.pids;
    receiverOptions.pidMode = options.pidMode;
    receiverOptions.fw1Path = options.fw1Path;
    receiverOptions.fw2Path = options.fw2Path;

    Receiver receiver(receiverOptions, &regLogger, &statusLog);
    receiver.runConfiguration();

    StreamSession session(receiver.usbBridge(), *receiver.demodulator(),
                          receiver.tuner(), options, &statusLog, &regLogger);
    session.run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "\nerror: " << error.what() << '\n';
    return 1;
  }
}
