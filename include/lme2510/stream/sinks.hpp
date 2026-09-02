#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lme2510/net/udp_socket.hpp"

namespace lme2510 {

class FileSink {
 public:
  FileSink() = default;
  ~FileSink();

  void open(const std::string& path);
  void write(const std::vector<uint8_t>& data);
  void close();
  bool isOpen() const { return !path_.empty(); }

 private:
  std::string path_;
  FILE* file_ = nullptr;
};

/// UDP sink for packetized TS (1316-byte datagrams).
class UdpSink {
 public:
  bool openTarget(const std::string& target);
  void send(const std::vector<uint8_t>& data);
  void close();
  bool isOpen() const { return socket_.isOpen(); }
  const std::string& target() const { return target_; }

 private:
  UdpSocket socket_;
  std::string host_;
  uint16_t port_ = 0;
  std::string target_;
};

/// Raw UDP sink: one EP 0x88 bulk frame per datagram.
using RawUdpSink = UdpSink;

}  // namespace lme2510
