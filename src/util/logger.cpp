#include "lme2510/util/logger.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace lme2510 {

std::string nowTimestamp() {
  using Clock = std::chrono::system_clock;
  const auto now = Clock::now();
  const auto time = Clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count() %
                  1000;

  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif

  std::ostringstream out;
  out << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
      << std::setfill('0') << std::setw(3) << ms;
  return out.str();
}

std::string hexByte(uint8_t v) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
      << static_cast<int>(v);
  return out.str();
}

std::string hexBytes(const std::vector<uint8_t>& data, const char* separator) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  bool first = true;
  for (uint8_t b : data) {
    if (!first) {
      out << separator;
    }
    first = false;
    out << std::setw(2) << static_cast<int>(b);
  }
  return out.str();
}

std::string hexWord(uint16_t v) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
      << static_cast<int>(v);
  return out.str();
}

std::string regValueText(int v) {
  if (v < 0) {
    return "--";
  }
  return "0x" + hexByte(static_cast<uint8_t>(v));
}

RegLogger::RegLogger(const std::string& path, bool usbTrace) {
  open(path, usbTrace);
}

void RegLogger::open(const std::string& path, bool usbTrace) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_.is_open()) {
    file_.close();
  }
  path_ = path;
  usbTrace_ = usbTrace;
  if (path.empty()) {
    return;
  }
  const std::filesystem::path fsPath(path);
  if (fsPath.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(fsPath.parent_path(), ec);
  }
  file_.open(path, std::ios::out | std::ios::app);
}

void RegLogger::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_.is_open()) {
    file_.close();
  }
  path_.clear();
}

bool RegLogger::enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_.is_open();
}

bool RegLogger::usbTrace() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return usbTrace_;
}

void RegLogger::log(const std::string& kind, const std::string& detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_.is_open()) {
    return;
  }
  file_ << nowTimestamp() << " | " << kind << " | " << detail << '\n';
  file_.flush();
}

}  // namespace lme2510
