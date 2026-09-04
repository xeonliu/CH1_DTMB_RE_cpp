#include "lme2510/tui/capture_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <thread>
#include <utility>

#include "lme2510/bridge/usb_bridge.hpp"
#include "lme2510/util/logger.hpp"

namespace lme2510::tui {

namespace {

constexpr std::size_t kUdpPayloadBytes = 1316;
constexpr std::size_t kMaxStatusBytes = 4096;

std::uint32_t crc32Mpeg(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= static_cast<std::uint32_t>(data[index]) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000U) != 0
                ? (crc << 1) ^ 0x04C11DB7U
                : crc << 1;
    }
  }
  return crc;
}

void appendPatPacket(std::vector<std::uint8_t>& output,
                     const ServiceSelectionSnapshot& selection,
                     std::uint8_t& patCc) {
  std::vector<std::uint8_t> packet(kTsLen, 0xFF);
  packet[0] = kTsSync;
  packet[1] = 0x40;  // payload unit start, no TEI/PID extension bits
  packet[2] = 0x00;
  // AFC=01 (payload only) must be set in bits 5-4; a zero byte here is the
  // reserved AFC value and compliant demuxers (e.g. ffmpeg) drop the packet.
  packet[3] = static_cast<std::uint8_t>(0x10 | (patCc & 0x0F));
  patCc = static_cast<std::uint8_t>((patCc + 1) & 0x0F);

  // Build a one-program PAT section (program -> PMT PID).
  std::vector<std::uint8_t> section;
  section.push_back(0x00);                       // table_id = PAT
  // Bits 7-4: section_syntax_indicator=1, '0', reserved=11 (0xB).
  section.push_back(0xB0);                       // syntax + length high
  section.push_back(0x0D);                       // section_length low (13)
  section.push_back(0x00);                       // transport_stream_id hi
  section.push_back(0x01);                       // transport_stream_id lo
  section.push_back(0xC1);                       // version 0, current
  section.push_back(0x00);                       // section_number
  section.push_back(0x00);                       // last_section_number
  section.push_back(
      static_cast<std::uint8_t>(selection.programNumber >> 8));
  section.push_back(
      static_cast<std::uint8_t>(selection.programNumber & 0xFF));
  section.push_back(
      static_cast<std::uint8_t>(0xE0 | ((selection.pmtPid >> 8) & 0x1F)));
  section.push_back(static_cast<std::uint8_t>(selection.pmtPid & 0xFF));
  const std::uint32_t crc = crc32Mpeg(section.data(), section.size());
  section.push_back(static_cast<std::uint8_t>((crc >> 24) & 0xFF));
  section.push_back(static_cast<std::uint8_t>((crc >> 16) & 0xFF));
  section.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
  section.push_back(static_cast<std::uint8_t>(crc & 0xFF));

  packet[4] = 0x00;  // pointer_field
  std::copy(section.begin(), section.end(), packet.begin() + 5);
  output.insert(output.end(), packet.begin(), packet.end());
}

bool serviceAllowsPid(const ServiceSelectionSnapshot& selection,
                      std::uint16_t pid) {
  for (const std::uint16_t allowed : selection.pids) {
    if (allowed == pid) {
      return true;
    }
  }
  return false;
}

std::vector<std::uint8_t> filterPacketsForService(
    const std::vector<std::uint8_t>& source,
    const ServiceSelectionSnapshot& selection, std::uint8_t& patCc,
    std::size_t& packetsSincePat) {
  std::vector<std::uint8_t> output;
  output.reserve(source.size());
  if (packetsSincePat >= 50) {
    appendPatPacket(output, selection, patCc);
    packetsSincePat = 0;
  }
  for (std::size_t offset = 0; offset + kTsLen <= source.size();
       offset += kTsLen) {
    const std::uint8_t* packet = source.data() + offset;
    if (packet[0] != kTsSync) {
      continue;
    }
    const std::uint16_t pid =
        static_cast<std::uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid == 0x0000) {
      continue;  // synthesized PAT is inserted instead
    }
    if (!serviceAllowsPid(selection, pid)) {
      continue;
    }
    output.insert(output.end(), packet, packet + kTsLen);
    ++packetsSincePat;
  }
  return output;
}

std::string optionalHex(const std::optional<std::uint8_t>& value) {
  return value ? "0x" + hexByte(*value) : std::string("--");
}

}  // namespace

CaptureSession::CaptureSession(UsbBridge& usb, Demodulator* demod,
                               Max2165Tuner* tuner, const Options& options,
                               std::ostream* statusLog, RegLogger* regLogger,
                               TuiModel& model)
    : usb_(usb),
      demod_(demod),
      tuner_(tuner),
      options_(options),
      statusLog_(statusLog),
      regLogger_(regLogger),
      model_(model) {}

CaptureSession::~CaptureSession() {
  stop();
}

void CaptureSession::setForwarding(bool forwarding) {
  forwarding_.store(forwarding);
}

void CaptureSession::setRecording(bool recording) {
  recording_.store(recording);
}

void CaptureSession::setSelectedService(
    std::shared_ptr<const ServiceSelectionSnapshot> selection) {
  std::lock_guard<std::mutex> lock(selectionMutex_);
  selection_ = std::move(selection);
}

std::shared_ptr<const ServiceSelectionSnapshot>
CaptureSession::selectedService() const {
  std::lock_guard<std::mutex> lock(selectionMutex_);
  return selection_;
}

bool CaptureSession::running() const {
  return running_.load();
}

void CaptureSession::start() {
  stop_.store(false);
  forwarding_.store(false);
  recording_.store(false);
  running_.store(false);
  model_.resetCounters();
  model_.resetSignal();
  model_.clearTelemetry();
  model_.clearServices();
  {
    std::lock_guard<std::mutex> lock(selectionMutex_);
    selection_.reset();
  }
  guide_.clear();
  guideVersion_ = 0;

  if (!options_.noUdp) {
    udp_.openTarget(options_.udpTarget);
  }
  if (!options_.rawUdpTarget.empty()) {
    rawUdp_.openTarget(options_.rawUdpTarget);
  }

  running_.store(true);
  streamThread_ = std::thread([this] { streamLoop(); });
  statusThread_ = std::thread([this] { statusLoop(); });
}

void CaptureSession::stop() {
  stop_.store(true);
  if (streamThread_.joinable()) {
    streamThread_.join();
  }
  if (statusThread_.joinable()) {
    statusThread_.join();
  }
  running_.store(false);
  udp_.close();
  rawUdp_.close();
  file_.close();
  fileOpened_ = false;
}

void CaptureSession::streamLoop() {
  std::uint64_t ccErrors = 0;
  std::uint64_t ccLost = 0;
  std::uint64_t transportErrors = 0;

  double windowBytes = 0.0;
  double windowOutputBytes = 0.0;
  double lastRateReset = 0.0;
  double lastTelemetry = 0.0;
  std::uint8_t patCc = 0;
  std::size_t packetsSincePat = 0;
  const auto started = std::chrono::steady_clock::now();
  auto elapsed = [&]() -> double {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - started)
        .count();
  };

  try {
    while (!stop_.load()) {
      const double now = elapsed();
      if (options_.seconds > 0.0 && now >= options_.seconds) {
        break;
      }

      std::vector<std::uint8_t> frame =
          usb_.readStreamChunk(kMaxStatusBytes, 250);
      if (frame.empty()) {
        model_.addTimeout();
        if (options_.telemetryInterval > 0.0 &&
            now - lastTelemetry >= options_.telemetryInterval) {
          sampleTelemetry();
          lastTelemetry = now;
        }
        continue;
      }

      model_.addFrames();

      if (forwarding_.load() && rawUdp_.isOpen()) {
        rawUdp_.send(frame);
        model_.addRaw(frame.size(), 1);
      }

      const std::uint64_t resyncBefore = packetizer_.resyncCount();
      const std::uint64_t droppedBefore = packetizer_.droppedBytes();
      std::vector<std::uint8_t> packets = packetizer_.feed(frame);
      model_.addResync(packetizer_.resyncCount() - resyncBefore);
      model_.addDroppedBytes(packetizer_.droppedBytes() - droppedBefore);

      if (!packets.empty()) {
        const std::uint64_t count = packets.size() / kTsLen;
        windowBytes += static_cast<double>(packets.size());
        model_.addBytes(packets.size());
        model_.addPackets(count);

        continuity_.feed(packets.data(), packets.size());
        const std::uint64_t errorsNow = continuity_.ccErrors();
        const std::uint64_t lostNow = continuity_.lostPackets();
        const std::uint64_t transportNow = continuity_.transportErrors();
        const std::uint64_t errorsDelta = errorsNow - ccErrors;
        const std::uint64_t lostDelta = lostNow - ccLost;
        const std::uint64_t transportDelta = transportNow - transportErrors;
        ccErrors = errorsNow;
        ccLost = lostNow;
        transportErrors = transportNow;
        model_.addCcLoss(errorsDelta, lostDelta, transportDelta);

        guide_.feed(packets.data(), packets.size());
        if (guide_.version() != guideVersion_) {
          guideVersion_ = guide_.version();
          model_.setServices(guide_.services());
        }

        const bool sendStream = forwarding_.load();
        const bool record = recording_.load();
        if (sendStream || record) {
          std::vector<std::uint8_t> output;
          const std::shared_ptr<const ServiceSelectionSnapshot> selection =
              selectedService();
          if (selection != nullptr && selection->active) {
            output = filterPacketsForService(packets, *selection, patCc,
                                             packetsSincePat);
          } else {
            output.swap(packets);
          }
          windowOutputBytes += static_cast<double>(output.size());

          if (record) {
            ensureFileOpened();
            if (fileOpened_) {
              file_.write(output);
            }
          }
          if (sendStream && udp_.isOpen()) {
            std::uint64_t datagrams = 0;
            for (std::size_t off = 0;
                 off + kUdpPayloadBytes <= output.size(); off += kUdpPayloadBytes) {
              udp_.send(std::vector<std::uint8_t>(
                  output.begin() + static_cast<std::ptrdiff_t>(off),
                  output.begin() +
                      static_cast<std::ptrdiff_t>(off + kUdpPayloadBytes)));
              ++datagrams;
            }
            const std::size_t remainder = output.size() % kUdpPayloadBytes;
            if (remainder != 0) {
              udp_.send(std::vector<std::uint8_t>(
                  output.end() - static_cast<std::ptrdiff_t>(remainder),
                  output.end()));
              ++datagrams;
            }
            model_.addUdp(output.size(), datagrams);
          }
        }
      }

      const double current = elapsed();
      if (current - lastRateReset >= 0.5) {
        const double delta = current - lastRateReset;
        model_.setRateMbps(delta > 0.0
                               ? windowBytes * 8.0 / delta / 1e6
                               : 0.0);
        model_.setOutputRateMbps(
            delta > 0.0 ? windowOutputBytes * 8.0 / delta / 1e6 : 0.0);
        windowBytes = 0.0;
        windowOutputBytes = 0.0;
        lastRateReset = current;
      }
    }
  } catch (const std::exception& error) {
    setCaptureError(std::string("流读取中断: ") + error.what());
  }
}

void CaptureSession::statusLoop() {
  auto lastFileLog = std::chrono::steady_clock::now();
  try {
    while (!stop_.load()) {
      const std::vector<std::uint8_t> raw = usb_.readStatusTransfer(250);
      if (raw.empty()) {
        model_.addStatusSample(false);
        continue;
      }
      const std::optional<Ep8aStatus> parsed = parseFirstStatusPacket(raw);
      if (!parsed) {
        model_.addStatusSample(false);
        continue;
      }
      model_.addStatusSample(true);

      SignalSnapshot signal;
      signal.has = true;
      signal.lock = parsed->lock;
      signal.strengthPct = parsed->strengthPercent;
      signal.qualityPct = parsed->qualityPercent;
      signal.signalRaw = parsed->signalLevel;
      signal.snrRaw = parsed->snrRaw;
      signal.hi = parsed->hi;
      signal.lo = parsed->lo;
      signal.hits = model_.statusHits();
      signal.misses = model_.statusMisses();
      model_.setSignal(signal);

      const auto now = std::chrono::steady_clock::now();
      const double sinceLog =
          std::chrono::duration<double>(now - lastFileLog).count();
      if (sinceLog >= 1.0) {
        logLine("STATUS " + ep8aStatusLine(*parsed));
        lastFileLog = now;
      }
    }
  } catch (const std::exception& error) {
    setCaptureError(std::string("状态读取中断: ") + error.what());
  }
}

void CaptureSession::sampleTelemetry() {
  if (demod_ == nullptr || tuner_ == nullptr) {
    return;
  }
  const auto fmt = [](const std::optional<std::uint8_t>& value) {
    return optionalHex(value);
  };
  const std::optional<std::uint8_t> reg4b = demod_->readRegister(0x4B);
  const std::optional<std::uint8_t> rega4 = demod_->readRegister(0xA4);
  const std::optional<std::uint8_t> reg37 = demod_->readRegister(0x37);
  const std::optional<std::uint8_t> reg7c = demod_->readRegister(0x7C);
  const std::optional<std::uint8_t> rega2 = demod_->readRegister(0xA2);
  const std::optional<std::uint8_t> tuner11 = tuner_->readRegister(0x11);
  const std::optional<std::uint8_t> tuner12 = tuner_->readRegister(0x12);

  std::ostringstream detail;
  detail << "4B=" << fmt(reg4b) << " A4=" << fmt(rega4)
         << " 37=" << fmt(reg37) << " 7C=" << fmt(reg7c)
         << " A2=" << fmt(rega2) << " tuner11=" << fmt(tuner11)
         << " tuner12=" << fmt(tuner12);

  TelemetrySnapshot telemetry;
  telemetry.has = true;
  telemetry.detail = detail.str();
  model_.setTelemetry(telemetry);

  if (regLogger_ != nullptr && regLogger_->enabled()) {
    regLogger_->log("TELEMETRY", detail.str());
  }
}

void CaptureSession::ensureFileOpened() {
  if (fileOpened_) {
    return;
  }
  std::string path = options_.filePath;
  if (path.empty()) {
    path = "record-" + std::to_string(static_cast<long long>(std::time(nullptr))) +
           ".ts";
  }
  file_.open(path);
  fileOpened_ = true;
  model_.setRecordPath(path);
  logLine("File capture: " + path);
}

void CaptureSession::logLine(const std::string& text) {
  std::lock_guard<std::mutex> lock(logMutex_);
  if (statusLog_ != nullptr) {
    *statusLog_ << nowTimestamp() << " | " << text << '\n';
    statusLog_->flush();
  }
}

void CaptureSession::setCaptureError(const std::string& text) {
  logLine(text);
  model_.setError(text);
}

}  // namespace lme2510::tui
