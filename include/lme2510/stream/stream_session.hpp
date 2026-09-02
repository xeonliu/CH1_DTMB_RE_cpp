#pragma once

#include <atomic>
#include <iosfwd>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lme2510/demod/demodulator.hpp"
#include "lme2510/stream/sinks.hpp"
#include "lme2510/stream/ts_packetizer.hpp"
#include "lme2510/tuner/max2165.hpp"
#include "lme2510/util/arg_parser.hpp"

namespace lme2510 {

class RegLogger;
class UsbBridge;

/// EP 0x88 forwarding loop plus optional EP 0x8A background reader and
/// periodic register telemetry.
class StreamSession {
 public:
  StreamSession(UsbBridge& usb, Demodulator& demod, Max2165Tuner& tuner,
                const Options& options, std::ostream* statusLog,
                RegLogger* regLogger);
  ~StreamSession();

  void run();

 private:
  void say(const std::string& line);
  void sampleOnce();
  void sampleTelemetry();
  void statusReaderLoop();

  UsbBridge& usb_;
  Demodulator& demod_;
  Max2165Tuner& tuner_;
  const Options& options_;
  std::ostream* statusLog_;
  RegLogger* regLogger_;

  std::mutex sayMutex_;
  std::thread statusThread_;
  std::atomic<bool> statusStop_{false};
  std::atomic<std::uint64_t> statusHits_{0};
  std::atomic<std::uint64_t> statusMisses_{0};

  UdpSink udp_;
  RawUdpSink rawUdp_;
  FileSink file_;
};

}  // namespace lme2510
