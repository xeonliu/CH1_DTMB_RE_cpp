#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lme2510 {

/// One logical service/program found in a DTMB transport stream.
struct MuxService {
  std::uint16_t programNumber = 0;
  std::uint16_t pmtPid = 0;
  std::uint16_t pcrPid = 0;
  std::string name;      // from SDT when present, otherwise "Program N"
  std::string typeName;  // from SDT service type, or stream-type summary
  std::vector<std::uint16_t> streamPids;  // PCR + elementary PIDs (no PMT)
};

/// Incremental PAT/PMT/SDT parser for the multiplex currently being captured.
/// Not thread-safe; feed it from one TS consumer thread.
class ProgramGuide {
 public:
  ProgramGuide() = default;

  void feedPacket(const std::uint8_t* packet);
  void feed(const std::uint8_t* packets, std::size_t bytes);
  void clear();

  const std::vector<MuxService>& services() const { return services_; }
  std::size_t version() const { return version_; }
  std::uint16_t transportStreamId() const { return transportStreamId_; }

 private:
  void collectPid(std::uint16_t pid, bool unitStart,
                  const std::uint8_t* payload, std::size_t size);
  void tryParseSection(std::uint16_t pid);
  void rebuild();

  struct SectionBuffer {
    std::vector<std::uint8_t> bytes;
  };
  struct ProgramInfo {
    std::uint16_t pmtPid = 0;
    std::uint16_t pcrPid = 0;
    std::vector<std::uint16_t> streamPids;
    std::string name;
    std::string typeName;
  };

  std::map<std::uint16_t, SectionBuffer> sections_;
  std::map<std::uint16_t, ProgramInfo> programs_;
  std::vector<MuxService> services_;
  std::uint16_t transportStreamId_ = 1;
  std::size_t version_ = 0;
};

}  // namespace lme2510
