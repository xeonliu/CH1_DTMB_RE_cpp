#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
using NativeSocket = SOCKET;
#else
using NativeSocket = int;
#endif

namespace lme2510 {

/// Tiny BSD/WinSock UDP socket wrapper used by the UDP sinks.
class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  void create();
  void setSendBuffer(std::size_t bytes);
  void sendTo(const std::string& host, uint16_t port, const void* data,
              std::size_t size);
  void close();
  bool isOpen() const { return fd_ != invalidSocket(); }

 private:
  static NativeSocket invalidSocket();
  NativeSocket fd_;
};

/// Splits "host:port" using the last colon.  Returns false on malformed input.
bool splitHostPort(const std::string& target, std::string& host,
                   uint16_t& port);

}  // namespace lme2510
