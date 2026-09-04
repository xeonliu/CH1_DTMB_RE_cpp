#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lme2510 {

/// Continuity-counter loss monitor for a 188-byte MPEG-TS packet stream.
///
/// Tracks the 4-bit continuity counter per PID and reports discontinuity
/// events plus an estimate of lost packets.  Adaptation-field discontinuity
/// indicators are honoured so PCR resets do not count as losses.
class TsContinuityMonitor {
 public:
  TsContinuityMonitor();

  /// Expects *bytes* to be a whole number of complete 188-byte TS packets.
  void feed(const uint8_t* data, std::size_t bytes);

  void reset();

  std::uint64_t trackedPackets() const { return trackedPackets_; }
  std::uint64_t ccErrors() const { return ccErrors_; }
  std::uint64_t lostPackets() const { return lostPackets_; }
  std::uint64_t transportErrors() const { return transportErrors_; }

 private:
  void checkPacket(const uint8_t* packet);

  std::array<std::uint8_t, 0x2000> lastCc_;
  std::uint64_t trackedPackets_ = 0;
  std::uint64_t ccErrors_ = 0;
  std::uint64_t lostPackets_ = 0;
  std::uint64_t transportErrors_ = 0;
};

}  // namespace lme2510
