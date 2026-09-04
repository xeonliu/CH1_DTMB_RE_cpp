#pragma once

#include <atomic>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lme2510/demod/demodulator.hpp"
#include "lme2510/stream/sinks.hpp"
#include "lme2510/stream/program_guide.hpp"
#include "lme2510/stream/ts_loss.hpp"
#include "lme2510/stream/ts_packetizer.hpp"
#include "lme2510/tui/tui_model.hpp"
#include "lme2510/tuner/max2165.hpp"
#include "lme2510/util/arg_parser.hpp"

namespace lme2510 {

class RegLogger;
class UsbBridge;

namespace tui {

/// Continuously drains EP 0x88 (always) and EP 0x8A (status) while the TUI is
/// monitoring.  Forwarding to UDP/raw-UDP/file is gated by setForwarding() so
/// the user can watch signal quality before starting the stream.
class CaptureSession {
 public:
  CaptureSession(UsbBridge& usb, Demodulator* demod, Max2165Tuner* tuner,
                 const Options& options, std::ostream* statusLog,
                 RegLogger* regLogger, TuiModel& model);
  ~CaptureSession();

  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;

  void start();
  void stop();
  void setForwarding(bool forwarding);
  void setRecording(bool recording);
  void setSelectedService(std::shared_ptr<const ServiceSelectionSnapshot> sel);
  bool forwarding() const { return forwarding_.load(); }
  bool recording() const { return recording_.load(); }
  std::shared_ptr<const ServiceSelectionSnapshot> selectedService() const;
  bool running() const;

 private:
  void streamLoop();
  void statusLoop();
  void sampleTelemetry();
  void ensureFileOpened();
  void logLine(const std::string& text);
  void setCaptureError(const std::string& text);

  UsbBridge& usb_;
  Demodulator* demod_;
  Max2165Tuner* tuner_;
  const Options& options_;
  std::ostream* statusLog_;
  RegLogger* regLogger_;
  TuiModel& model_;

  std::atomic<bool> stop_{false};
  std::atomic<bool> forwarding_{false};
  std::atomic<bool> recording_{false};
  std::atomic<bool> running_{false};
  mutable std::mutex selectionMutex_;
  std::shared_ptr<const ServiceSelectionSnapshot> selection_;
  std::thread streamThread_;
  std::thread statusThread_;
  mutable std::mutex logMutex_;

  UdpSink udp_;
  RawUdpSink rawUdp_;
  FileSink file_;
  bool fileOpened_ = false;
  TsPacketizer packetizer_;
  TsContinuityMonitor continuity_;
  ProgramGuide guide_;
  std::size_t guideVersion_ = 0;
};

}  // namespace tui
}  // namespace lme2510
