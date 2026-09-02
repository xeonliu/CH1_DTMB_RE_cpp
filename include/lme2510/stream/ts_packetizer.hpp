#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lme2510 {

inline constexpr std::size_t kTsLen = 188;
inline constexpr uint8_t kTsSync = 0x47;

/// Converts the ordered EP 0x88 bulk stream into a continuous 188-byte TS
/// packet stream.  Keeps partial packets that cross USB frame boundaries and
/// re-synchronizes only after a real alignment failure.
class TsPacketizer {
 public:
  explicit TsPacketizer(int minSync = 3);

  std::vector<uint8_t> feed(const uint8_t* data, std::size_t size);
  std::vector<uint8_t> feed(const std::vector<uint8_t>& data);

  std::size_t packetCount() const { return packets_; }
  std::size_t resyncCount() const { return resyncs_; }
  std::size_t droppedBytes() const { return droppedBytes_; }
  const std::vector<uint8_t>& buffer() const { return buffer_; }

 private:
  bool advanceToSync();

  std::vector<uint8_t> buffer_;
  int minSync_;
  std::size_t packets_ = 0;
  std::size_t resyncs_ = 0;
  std::size_t droppedBytes_ = 0;
};

/// Returns (offset, hits) for the 188-byte alignment with the most sync bytes.
std::pair<std::size_t, std::size_t> bestSyncOffset(
    const std::vector<uint8_t>& frame, int minSync = 3);

}  // namespace lme2510
