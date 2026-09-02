#pragma once

#include <string>

namespace lme2510 {

struct Options {
  int frequencyMhz = 618;
  std::string pids;
  int pidMode = 0;
  std::string udpTarget = "127.0.0.1:1234";
  std::string rawUdpTarget;
  bool noUdp = false;
  std::string filePath;
  double seconds = 0.0;
  double telemetryInterval = 2.0;
  bool liveStatus = false;
  std::string statusLogPath;
  std::string regLogPath;
  bool usbTrace = false;
  std::string fw1Path;
  std::string fw2Path;
};

void printHelp();
std::string helpText();

/// Parses command line arguments in Python argparse-compatible style.  Returns
/// false and fills *error on failure; --help sets *showHelp.
bool parseArgs(int argc, char** argv, Options& out, std::string& error,
               bool& showHelp);

}  // namespace lme2510
