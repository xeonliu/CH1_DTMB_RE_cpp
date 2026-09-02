#include "lme2510/stream/stream_session.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

#include "lme2510/bridge/usb_bridge.hpp"
#include "lme2510/util/logger.hpp"
#include "lme2510/util/platform.hpp"

namespace lme2510 {
namespace {

constexpr std::size_t kUdpPayloadBytes = 1316;

}  // namespace

StreamSession::StreamSession(UsbBridge& usb, Demodulator& demod,
                             Max2165Tuner& tuner, const Options& options,
                             std::ostream* statusLog, RegLogger* regLogger)
    : usb_(usb),
      demod_(demod),
      tuner_(tuner),
      options_(options),
      statusLog_(statusLog),
      regLogger_(regLogger) {}

StreamSession::~StreamSession() {
  statusStop_.store(true);
  if (statusThread_.joinable()) {
    statusThread_.join();
  }
  udp_.close();
  rawUdp_.close();
  file_.close();
}

void StreamSession::say(const std::string& line) {
  std::lock_guard<std::mutex> lock(sayMutex_);
  std::cout << line << '\n';
  if (statusLog_ != nullptr) {
    *statusLog_ << nowTimestamp() << " | " << line << '\n';
    statusLog_->flush();
  }
}

void StreamSession::sampleTelemetry() {
  const auto fmt = [](std::optional<uint8_t> value) {
    return value ? "0x" + hexByte(*value) : std::string("--");
  };
  const std::optional<uint8_t> reg02 = demod_.readRegister(0x02);
  const std::optional<uint8_t> reg04 = demod_.readRegister(0x04);
  const std::optional<uint8_t> reg37 = demod_.readRegister(0x37);
  const std::optional<uint8_t> reg4b = demod_.readRegister(0x4B);
  const std::optional<uint8_t> reg7c = demod_.readRegister(0x7C);
  const std::optional<uint8_t> reg7e = demod_.readRegister(0x7E);
  const std::optional<uint8_t> rega2 = demod_.readRegister(0xA2);
  const std::optional<uint8_t> rega4 = demod_.readRegister(0xA4);
  const std::optional<uint8_t> regc5 = demod_.readRegister(0xC5);
  const std::optional<uint8_t> tuner11 = tuner_.readRegister(0x11);
  const std::optional<uint8_t> tuner12 = tuner_.readRegister(0x12);

  if (regLogger_ != nullptr && regLogger_->enabled()) {
    std::ostringstream detail;
    detail << "4B=" << fmt(reg4b) << " A4=" << fmt(rega4)
           << " 37=" << fmt(reg37) << " 7C=" << fmt(reg7c)
           << " A2=" << fmt(rega2) << " tuner11=" << fmt(tuner11)
           << " tuner12=" << fmt(tuner12);
    regLogger_->log("TELEMETRY", detail.str());
  }
  // Prevent -Wunused warnings for registers the Python snapshot also reads but
  // that are not part of the one-line summary (kept for traceability).
  (void)reg02;
  (void)reg04;
  (void)reg7e;
  (void)regc5;
}

void StreamSession::sampleOnce() {
  const std::vector<uint8_t> raw = usb_.readStatusTransfer(120);
  const std::optional<Ep8aStatus> status = parseFirstStatusPacket(raw);
  if (status) {
    say("STATUS " + ep8aStatusLine(*status));
  }
  if (options_.telemetryInterval > 0.0) {
    sampleTelemetry();
  }
}

void StreamSession::statusReaderLoop() {
  while (!statusStop_.load()) {
    const std::vector<uint8_t> raw = usb_.readStatusTransfer(250);
    if (raw.empty() || !parseFirstStatusPacket(raw)) {
      ++statusMisses_;
      continue;
    }
    ++statusHits_;
    const auto status = parseFirstStatusPacket(raw);
    if (status) {
      say("STATUS " + ep8aStatusLine(*status));
    }
  }
}

void StreamSession::run() {
  if (!options_.noUdp) {
    udp_.openTarget(options_.udpTarget);
    std::cout << "UDP target: " << options_.udpTarget << '\n';
  }
  if (!options_.rawUdpTarget.empty()) {
    rawUdp_.openTarget(options_.rawUdpTarget);
    std::cout << "Raw UDP target: " << options_.rawUdpTarget
              << " (one datagram per EP 0x88 bulk frame)\n";
  }
  if (!options_.filePath.empty()) {
    file_.open(options_.filePath);
    std::cout << "File capture: " << options_.filePath << '\n';
  }

  TsPacketizer packetizer;
  const auto start = std::chrono::steady_clock::now();
  std::size_t bytesTotal = 0;
  std::size_t packetsTotal = 0;
  std::size_t framesTotal = 0;
  std::size_t timeouts = 0;
  std::size_t rawBytesTotal = 0;
  std::size_t rawDatagramsTotal = 0;
  double lastIdleSample = 0.0;

  auto elapsedSeconds = [&]() -> double {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start).count();
  };

  if (options_.liveStatus) {
    statusThread_ = std::thread([this] { statusReaderLoop(); });
  }

  try {
    while (!stopRequested().load()) {
      const double elapsed = elapsedSeconds();
      if (options_.seconds > 0.0 && elapsed >= options_.seconds) {
        break;
      }

      std::vector<uint8_t> frame = usb_.readStreamChunk(4096, 500);
      if (!frame.empty()) {
        ++framesTotal;
        if (rawUdp_.isOpen()) {
          rawUdp_.send(frame);
          rawBytesTotal += frame.size();
          ++rawDatagramsTotal;
        }

        std::vector<uint8_t> packets = packetizer.feed(frame);
        if (!packets.empty()) {
          const std::size_t count = packets.size() / kTsLen;
          bytesTotal += packets.size();
          packetsTotal += count;
          if (file_.isOpen()) {
            file_.write(packets);
          }
          if (udp_.isOpen()) {
            for (std::size_t off = 0; off + kUdpPayloadBytes <= packets.size();
                 off += kUdpPayloadBytes) {
              udp_.send(std::vector<uint8_t>(
                  packets.begin() + static_cast<std::ptrdiff_t>(off),
                  packets.begin() +
                      static_cast<std::ptrdiff_t>(off + kUdpPayloadBytes)));
            }
            const std::size_t remainder = packets.size() % kUdpPayloadBytes;
            if (remainder != 0) {
              udp_.send(std::vector<uint8_t>(
                  packets.end() - static_cast<std::ptrdiff_t>(remainder),
                  packets.end()));
            }
          }
        }
      } else {
        ++timeouts;
        const double now = elapsedSeconds();
        if (options_.telemetryInterval > 0.0 &&
            now - lastIdleSample >= options_.telemetryInterval) {
          sampleOnce();
          lastIdleSample = now;
        }
      }
    }
  } catch (const UsbError& error) {
    say(std::string("Interrupted by USB error: ") + error.what());
  }

  if (statusThread_.joinable()) {
    statusStop_.store(true);
    statusThread_.join();
  }

  const double elapsed = elapsedSeconds();
  sampleOnce();
  if (options_.liveStatus) {
    say("STATUS_THREAD samples=" +
        std::to_string(statusHits_.load() + statusMisses_.load()) +
        " hits=" + std::to_string(statusHits_.load()) +
        " misses=" + std::to_string(statusMisses_.load()));
  }

  std::ostringstream finalLine;
  finalLine << "FINAL elapsed=" << elapsed << "s bytes=" << bytesTotal
            << " pkts=" << packetsTotal << " frames=" << framesTotal
            << " timeouts=" << timeouts
            << " resyncs=" << packetizer.resyncCount()
            << " dropped_bytes=" << packetizer.droppedBytes();
  if (elapsed > 0.0) {
    finalLine << " rate=" << (bytesTotal * 8.0 / elapsed / 1e6) << " Mbit/s";
  }
  say(finalLine.str());

  if (rawUdp_.isOpen()) {
    say("RAW FINAL bytes=" + std::to_string(rawBytesTotal) +
        " datagrams=" + std::to_string(rawDatagramsTotal));
  }

  file_.close();
  udp_.close();
  rawUdp_.close();
}

}  // namespace lme2510
