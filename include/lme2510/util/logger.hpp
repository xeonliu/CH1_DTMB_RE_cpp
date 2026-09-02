#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace lme2510 {

std::string nowTimestamp();
std::string hexByte(uint8_t v);
std::string hexBytes(const std::vector<uint8_t>& data, const char* separator = " ");
std::string hexWord(uint16_t v);
std::string regValueText(int v);  // "--" when v < 0, otherwise "0xNN"

/// Human-readable register/status log.  Each line mirrors the Python port's
/// "<timestamp> | KIND | detail" format and is written to one file with a lock.
class RegLogger {
 public:
  RegLogger() = default;
  RegLogger(const std::string& path, bool usbTrace);

  void open(const std::string& path, bool usbTrace);
  void close();
  bool enabled() const;
  bool usbTrace() const;

  void log(const std::string& kind, const std::string& detail);

 private:
  mutable std::mutex mutex_;
  std::ofstream file_;
  std::string path_;
  bool usbTrace_ = false;
};

}  // namespace lme2510
