#include "lme2510/net/udp_socket.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lme2510 {

UdpSocket::UdpSocket() : fd_(invalidSocket()) {}

UdpSocket::~UdpSocket() {
  close();
}

NativeSocket UdpSocket::invalidSocket() {
#ifdef _WIN32
  return INVALID_SOCKET;
#else
  return -1;
#endif
}

void UdpSocket::create() {
  close();
  fd_ = static_cast<NativeSocket>(
      ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (fd_ == invalidSocket()) {
#ifdef _WIN32
    throw std::runtime_error("socket() failed: " +
                             std::to_string(WSAGetLastError()));
#else
    throw std::runtime_error("socket() failed: " +
                             std::string(std::strerror(errno)));
#endif
  }
}

void UdpSocket::setSendBuffer(std::size_t bytes) {
  const int value = static_cast<int>(bytes);
  ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&value), sizeof(value));
}

void UdpSocket::sendTo(const std::string& host, uint16_t port, const void* data,
                       std::size_t size) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    throw std::runtime_error("invalid UDP host: '" + host + "'");
  }

  const int sent = static_cast<int>(::sendto(
      fd_, static_cast<const char*>(data), static_cast<int>(size), 0,
      reinterpret_cast<const sockaddr*>(&address), sizeof(address)));
  if (sent < 0) {
#ifdef _WIN32
    const std::string what = "sendto() failed: " +
                             std::to_string(WSAGetLastError());
#else
    const std::string what = "sendto() failed: " +
                             std::string(std::strerror(errno));
#endif
    throw std::runtime_error(what);
  }
}

void UdpSocket::close() {
  if (fd_ == invalidSocket()) {
    return;
  }
#ifdef _WIN32
  ::closesocket(fd_);
#else
  ::close(fd_);
#endif
  fd_ = invalidSocket();
}

bool splitHostPort(const std::string& target, std::string& host,
                   uint16_t& port) {
  const std::size_t colon = target.rfind(':');
  if (colon == std::string::npos || colon == 0 ||
      colon + 1 == target.size()) {
    return false;
  }
  host = target.substr(0, colon);
  try {
    std::size_t pos = 0;
    const long value = std::stol(target.substr(colon + 1), &pos, 10);
    if (pos != target.size() - colon - 1 || value <= 0 || value > 65535) {
      return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace lme2510
