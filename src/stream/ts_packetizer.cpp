#include "lme2510/stream/ts_packetizer.hpp"

#include <algorithm>
#include <utility>

namespace lme2510 {

TsPacketizer::TsPacketizer(int minSync) : minSync_(minSync) {}

std::pair<std::size_t, std::size_t> bestSyncOffset(
    const std::vector<uint8_t>& frame, int minSync) {
  const std::size_t needed = kTsLen * static_cast<std::size_t>(minSync);
  if (frame.size() < needed) {
    return {0, 0};
  }
  std::size_t bestOffset = 0;
  std::size_t bestHits = 0;
  for (std::size_t off = 0; off < kTsLen; ++off) {
    std::size_t hits = 0;
    for (std::size_t start = off; start < frame.size(); start += kTsLen) {
      if (frame[start] == kTsSync) {
        ++hits;
      }
    }
    if (hits > bestHits) {
      bestHits = hits;
      bestOffset = off;
    }
  }
  return {bestOffset, bestHits};
}

std::vector<uint8_t> TsPacketizer::feed(const uint8_t* data, std::size_t size) {
  buffer_.insert(buffer_.end(), data, data + size);

  std::vector<uint8_t> output;
  while (buffer_.size() >= kTsLen) {
    if (buffer_.front() == kTsSync) {
      output.insert(output.end(), buffer_.begin(), buffer_.begin() + kTsLen);
      buffer_.erase(buffer_.begin(), buffer_.begin() + kTsLen);
      ++packets_;
      continue;
    }
    if (!advanceToSync()) {
      break;
    }
  }
  return output;
}

std::vector<uint8_t> TsPacketizer::feed(const std::vector<uint8_t>& data) {
  return feed(data.data(), data.size());
}

bool TsPacketizer::advanceToSync() {
  const std::size_t need =
      kTsLen * static_cast<std::size_t>(std::max(minSync_, 1));
  if (buffer_.size() < need) {
    return false;
  }

  const auto [offset, hits] = bestSyncOffset(buffer_, minSync_);
  if (hits >= static_cast<std::size_t>(std::max(minSync_, 1)) && offset > 0) {
    ++resyncs_;
    droppedBytes_ += offset;
    buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
    return true;
  }

  // No credible phase in the current window: drop one packet-width so long
  // garbage runs cannot make the buffer grow without bound.
  ++resyncs_;
  droppedBytes_ += kTsLen;
  buffer_.erase(buffer_.begin(), buffer_.begin() + kTsLen);
  return true;
}

}  // namespace lme2510
