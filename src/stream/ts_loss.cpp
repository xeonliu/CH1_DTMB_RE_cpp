#include "lme2510/stream/ts_loss.hpp"

#include "lme2510/stream/ts_packetizer.hpp"

namespace lme2510 {

namespace {

constexpr std::uint8_t kUnseenCc = 0x10;

}  // namespace

TsContinuityMonitor::TsContinuityMonitor() {
  lastCc_.fill(kUnseenCc);
}

void TsContinuityMonitor::reset() {
  lastCc_.fill(kUnseenCc);
  trackedPackets_ = 0;
  ccErrors_ = 0;
  lostPackets_ = 0;
  transportErrors_ = 0;
}

void TsContinuityMonitor::feed(const uint8_t* data, std::size_t bytes) {
  const std::size_t count = bytes / kTsLen;
  for (std::size_t index = 0; index < count; ++index) {
    checkPacket(data + index * kTsLen);
  }
}

void TsContinuityMonitor::checkPacket(const uint8_t* packet) {
  if (packet[0] != kTsSync) {
    return;
  }
  if ((packet[1] & 0x80) != 0) {
    ++transportErrors_;
  }

  const std::uint16_t pid =
      static_cast<std::uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
  const std::uint8_t cc = packet[3] & 0x0F;

  // Adaptation field discontinuity indicator resets the continuity counter.
  if ((packet[3] & 0x20) != 0) {
    const std::uint8_t adaptationLength = packet[4];
    if (adaptationLength > 0 && (packet[5] & 0x80) != 0) {
      lastCc_[pid] = cc;
      ++trackedPackets_;
      return;
    }
  }

  const std::uint8_t previous = lastCc_[pid];
  if (previous == kUnseenCc) {
    lastCc_[pid] = cc;
    ++trackedPackets_;
    return;
  }

  const std::uint8_t expected = static_cast<std::uint8_t>((previous + 1) & 0x0F);
  if (cc != expected) {
    ++ccErrors_;
    const std::uint8_t diff =
        static_cast<std::uint8_t>((cc - expected + 16) & 0x0F);
    lostPackets_ += diff == 0 ? 1 : diff;
  }
  lastCc_[pid] = cc;
  ++trackedPackets_;
}

}  // namespace lme2510
