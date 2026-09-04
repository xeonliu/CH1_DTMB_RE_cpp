#include "lme2510/stream/program_guide.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "lme2510/stream/ts_packetizer.hpp"

namespace lme2510 {

namespace {

constexpr std::uint16_t kPatPid = 0x0000;
constexpr std::uint16_t kSdtPid = 0x0011;

std::uint16_t be16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                    bytes[1]);
}

bool extractPayload(const std::uint8_t* packet, const std::uint8_t** payload,
                    std::size_t* size) {
  if (packet[0] != kTsSync || (packet[1] & 0x80) != 0) {
    return false;
  }
  const unsigned int afc = (packet[3] >> 4) & 0x03;
  if (afc == 0) {
    return false;
  }
  std::size_t offset = 4;
  if ((afc & 0x02) != 0) {
    const std::size_t length = packet[4];
    if (length > 183) {
      return false;
    }
    offset += 1 + length;
  }
  if ((afc & 0x01) == 0 || offset > kTsLen) {
    return false;
  }
  *payload = packet + offset;
  *size = kTsLen - offset;
  return true;
}

std::string bytesToString(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    const char byte = static_cast<char>(data[index]);
    out.push_back(byte == '\0' ? ' ' : byte);
  }
  return out;
}

}  // namespace

void ProgramGuide::feedPacket(const std::uint8_t* packet) {
  const std::uint16_t pid =
      static_cast<std::uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);

  bool collect = pid == kPatPid || pid == kSdtPid;
  if (!collect) {
    for (const auto& item : programs_) {
      if (item.second.pmtPid == pid) {
        collect = true;
        break;
      }
    }
  }
  if (!collect) {
    return;
  }

  const std::uint8_t* payload = nullptr;
  std::size_t size = 0;
  if (!extractPayload(packet, &payload, &size)) {
    return;
  }
  const bool unitStart = (packet[1] & 0x40) != 0;
  collectPid(pid, unitStart, payload, size);
}

void ProgramGuide::feed(const std::uint8_t* packets, std::size_t bytes) {
  const std::size_t count = bytes / kTsLen;
  for (std::size_t index = 0; index < count; ++index) {
    feedPacket(packets + index * kTsLen);
  }
}

void ProgramGuide::clear() {
  sections_.clear();
  programs_.clear();
  services_.clear();
  ++version_;
}

void ProgramGuide::collectPid(std::uint16_t pid, bool unitStart,
                              const std::uint8_t* payload, std::size_t size) {
  if (size == 0) {
    return;
  }
  SectionBuffer& buffer = sections_[pid];
  if (unitStart) {
    const std::size_t pointer = payload[0];
    buffer.bytes.clear();
    if (size > 1 + pointer) {
      buffer.bytes.insert(buffer.bytes.end(), payload + 1 + pointer,
                          payload + size);
    }
  } else {
    buffer.bytes.insert(buffer.bytes.end(), payload, payload + size);
  }
  tryParseSection(pid);
}

void ProgramGuide::tryParseSection(std::uint16_t pid) {
  std::vector<std::uint8_t>& bytes = sections_[pid].bytes;
  if (bytes.size() < 3) {
    return;
  }
  const std::size_t sectionLength =
      static_cast<std::size_t>(((bytes[1] & 0x0F) << 8) | bytes[2]);
  const std::size_t total = 3 + sectionLength;
  if (bytes.size() < total) {
    return;
  }

  const std::uint8_t tableId = bytes[0];
  if (pid == kPatPid && tableId == 0x00 && total >= 12) {
    transportStreamId_ = be16(bytes.data() + 3);
    std::size_t offset = 8;
    bool changed = false;
    while (offset + 4 <= total) {
      const std::uint16_t program = be16(bytes.data() + offset);
      const std::uint16_t pmtPid = be16(bytes.data() + offset + 2) & 0x1FFF;
      offset += 4;
      if (program == 0) {
        continue;
      }
      ProgramInfo& info = programs_[program];
      if (info.pmtPid != pmtPid) {
        info.pmtPid = pmtPid;
        changed = true;
      }
    }
    if (changed) {
      ++version_;
      rebuild();
    }
  } else if (pid != kPatPid && tableId == 0x02 && total >= 16) {
    const std::uint16_t program = be16(bytes.data() + 3);
    ProgramInfo& info = programs_[program];
    const std::uint16_t pcrPid = be16(bytes.data() + 8) & 0x1FFF;
    const std::size_t infoLength = be16(bytes.data() + 10) & 0x0FFF;
    std::size_t offset = 12 + infoLength;
    std::vector<std::uint16_t> pids;
    bool valid = offset <= total;
    while (valid && offset + 5 <= total) {
      const std::uint16_t esPid = be16(bytes.data() + offset + 1) & 0x1FFF;
      pids.push_back(esPid);
      const std::size_t esInfoLength =
          be16(bytes.data() + offset + 3) & 0x0FFF;
      offset += 5 + esInfoLength;
      valid = offset <= total;
    }
    if (valid) {
      info.pcrPid = pcrPid;
      info.streamPids = std::move(pids);
      auto it = std::find(info.streamPids.begin(), info.streamPids.end(),
                          pcrPid);
      if (pcrPid != 0 && it == info.streamPids.end()) {
        info.streamPids.push_back(pcrPid);
      }
      ++version_;
      rebuild();
    }
  } else if (pid == kSdtPid && tableId == 0x42 &&
             total >= 14) {
    std::size_t offset = 11;
    bool changed = false;
    while (offset + 5 <= total) {
      const std::uint16_t serviceId = be16(bytes.data() + offset);
      // Some DTMB SDT captures use a one-byte descriptors_loop_length here
      // (descriptor loop starts right after the running-status byte).  Probe
      // for the 0x48 service descriptor at both candidate offsets first.
      const bool oneByteLength =
          offset + 6 <= total && bytes[offset + 5] == 0x48;
      const std::size_t descriptorsLength =
          oneByteLength
              ? static_cast<std::size_t>(bytes[offset + 4])
              : (static_cast<std::size_t>(be16(bytes.data() + offset + 4) &
                                          0x0FFF));
      const std::size_t descriptorsEnd =
          offset + (oneByteLength ? 5 : 6) + descriptorsLength;
      if (descriptorsEnd > total) {
        break;
      }
      ProgramInfo& info = programs_[serviceId];
      std::size_t cursor = offset + (oneByteLength ? 5 : 6);
      while (cursor + 2 <= descriptorsEnd) {
        const std::uint8_t tag = bytes[cursor];
        const std::size_t length = bytes[cursor + 1];
        const std::size_t dataStart = cursor + 2;
        const std::size_t dataEnd = dataStart + length;
        if (dataEnd > descriptorsEnd) {
          break;
        }
        if (tag == 0x48 && length >= 3) {
          const std::size_t providerLength = bytes[dataStart + 1];
          const std::size_t nameOffset = dataStart + 2 + providerLength;
          if (nameOffset < dataEnd) {
            const std::size_t nameLength = bytes[nameOffset];
            const std::size_t nameStart = nameOffset + 1;
            if (nameStart + nameLength <= dataEnd) {
              const std::string name =
                  bytesToString(bytes.data() + nameStart, nameLength);
              if (info.name != name) {
                info.name = name;
                changed = true;
              }
              const std::uint8_t serviceType = bytes[dataStart];
              const std::string type =
                  serviceType == 0x01 || serviceType == 0x02
                      ? "TV"
                      : (serviceType == 0x0A ? "Radio" : "Service");
              if (info.typeName != type) {
                info.typeName = type;
                changed = true;
              }
            }
          }
        }
        cursor = dataEnd;
      }
      offset = descriptorsEnd;
    }
    if (changed) {
      ++version_;
      rebuild();
    }
  }
  bytes.clear();
}

void ProgramGuide::rebuild() {
  services_.clear();
  for (const auto& item : programs_) {
    const std::uint16_t program = item.first;
    const ProgramInfo& info = item.second;
    if (info.pmtPid == 0 || info.streamPids.empty()) {
      continue;
    }
    MuxService service;
    service.programNumber = program;
    service.pmtPid = info.pmtPid;
    service.pcrPid = info.pcrPid;
    service.streamPids = info.streamPids;
    service.name = info.name.empty()
                       ? "Program " + std::to_string(program)
                       : info.name;
    service.typeName =
        info.typeName.empty() ? "Program" : info.typeName;
    services_.push_back(service);
  }
  std::sort(services_.begin(), services_.end(),
            [](const MuxService& left, const MuxService& right) {
              return left.programNumber < right.programNumber;
            });
}

}  // namespace lme2510
