#include "lme2510/stream/sinks.hpp"

#include <cstdio>
#include <stdexcept>

namespace lme2510 {

FileSink::~FileSink() {
  close();
}

void FileSink::open(const std::string& path) {
  close();
#if defined(_MSC_VER)
  FILE* opened = nullptr;
  if (fopen_s(&opened, path.c_str(), "wb") != 0) {
    throw std::runtime_error("cannot open file: " + path);
  }
  file_ = opened;
#else
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    throw std::runtime_error("cannot open file: " + path);
  }
#endif
  path_ = path;
}

void FileSink::write(const std::vector<uint8_t>& data) {
  if (file_ == nullptr || data.empty()) {
    return;
  }
  const std::size_t written =
      std::fwrite(data.data(), 1, data.size(), file_);
  if (written != data.size()) {
    throw std::runtime_error("short write to file: " + path_);
  }
}

void FileSink::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
}

bool UdpSink::openTarget(const std::string& target) {
  if (!splitHostPort(target, host_, port_)) {
    throw std::runtime_error("invalid UDP target (expected host:port): '" +
                             target + "'");
  }
  socket_.create();
  socket_.setSendBuffer(1 << 20);
  target_ = target;
  return true;
}

void UdpSink::send(const std::vector<uint8_t>& data) {
  if (!socket_.isOpen() || data.empty()) {
    return;
  }
  socket_.sendTo(host_, port_, data.data(), data.size());
}

void UdpSink::close() {
  socket_.close();
  target_.clear();
}

}  // namespace lme2510
