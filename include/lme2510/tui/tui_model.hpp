#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "lme2510/demod/demodulator.hpp"
#include "lme2510/stream/program_guide.hpp"

namespace lme2510::tui {

/// One DTMB frequency row shown in the picker/scan list.
struct FreqEntry {
  int mhz = 0;
  bool checked = false;  // included in the next scan
  bool scanned = false;  // a scan/observation has produced a result
  bool locked = false;
  bool current = false;  // currently tuned/monitored
  int strengthPct = 0;
  int qualityPct = 0;
  double rateMbps = 0.0;
  std::uint64_t packets = 0;
  std::uint64_t ccErrors = 0;
  std::uint64_t ccLost = 0;
  std::string note;
};

/// Latest decoded EP 0x8A values.
struct SignalSnapshot {
  bool has = false;
  bool lock = false;
  int strengthPct = 0;
  int qualityPct = 0;
  int signalRaw = -1;
  int snrRaw = -1;
  int hi = -1;
  int lo = -1;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
};

/// Latest register telemetry (filled when a stream-read idle gap allows it).
struct TelemetrySnapshot {
  bool has = false;
  std::string detail;  // e.g. "4B=0xC4 A4=0x05 ..."
};

/// Service selected for PID-filtered output, or the whole-mux default.
struct ServiceSelectionSnapshot {
  bool active = false;
  std::uint16_t programNumber = 0;
  std::uint16_t pmtPid = 0;
  std::string name;
  std::vector<std::uint16_t> pids;  // PMT PID + PCR + elementary PIDs
};

/// Atomically-updated high-frequency receive/stream counters.
struct TsMetricsSnapshot {
  std::uint64_t bytes = 0;
  std::uint64_t packets = 0;
  std::uint64_t frames = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t resyncs = 0;
  std::uint64_t droppedBytes = 0;
  std::uint64_t ccErrors = 0;
  std::uint64_t ccLost = 0;
  std::uint64_t transportErrors = 0;
  std::uint64_t udpBytes = 0;
  std::uint64_t udpDatagrams = 0;
  std::uint64_t rawBytes = 0;
  std::uint64_t rawDatagrams = 0;
  double rateMbps = 0.0;
  double outputRateMbps = 0.0;
  double lossPct = 0.0;
};

/// Shared, thread-safe UI state.  The ncurses UI is the only reader; the
/// engine and its capture threads are the writers.
class TuiModel {
 public:
  TuiModel();

  void replaceFrequencies(const std::vector<int>& mhzList);
  std::vector<FreqEntry> frequencies() const;
  void setChecked(std::size_t index, bool checked);
  void updateFrequencyResult(int mhz, const FreqEntry& result);

  bool ready() const { return initialized_ && !fatal_; }
  bool fatal() const { return fatal_; }
  bool busy() const { return busy_; }
  bool monitoring() const { return monitoring_; }
  bool streaming() const { return streaming_; }
  bool recording() const { return recording_; }
  std::string recordPath() const;
  std::string phaseText() const { return phase_; }
  std::string errorText() const { return error_; }
  int currentMhz() const { return currentMhz_; }

  void setPhase(const std::string& text);
  void setBusy(bool busy);
  void setReady();
  void setFatal(const std::string& text);
  void setMonitoring(int mhz, bool monitoring);
  void setStreaming(bool streaming);
  void setRecording(bool recording);
  void setRecordPath(const std::string& path);
  void setServices(const std::vector<MuxService>& services);
  std::vector<MuxService> services() const;
  void clearServices();
  void setSelectedService(const ServiceSelectionSnapshot& service);
  ServiceSelectionSnapshot selectedService() const;
  void setError(const std::string& text);

  void setSignal(const SignalSnapshot& signal);
  SignalSnapshot signal() const;
  void resetSignal();
  void setTelemetry(const TelemetrySnapshot& telemetry);
  TelemetrySnapshot telemetry() const;
  void clearTelemetry();

  void setRateMbps(double rate);
  void setOutputRateMbps(double rate);
  void addBytes(std::uint64_t bytes);
  void addPackets(std::uint64_t packets);
  void addFrames(std::uint64_t frames = 1);
  void addTimeout();
  void addResync(std::uint64_t count = 1);
  void addDroppedBytes(std::uint64_t bytes);
  void addCcLoss(std::uint64_t errors, std::uint64_t lost,
                 std::uint64_t transportErrors);
  void addUdp(std::uint64_t bytes, std::uint64_t datagrams);
  void addRaw(std::uint64_t bytes, std::uint64_t datagrams);
  void addStatusSample(bool hit);
  void resetCounters();
  TsMetricsSnapshot counters() const;

 private:
  struct Counters {
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> resyncs{0};
    std::atomic<std::uint64_t> droppedBytes{0};
    std::atomic<std::uint64_t> ccErrors{0};
    std::atomic<std::uint64_t> ccLost{0};
    std::atomic<std::uint64_t> transportErrors{0};
    std::atomic<std::uint64_t> udpBytes{0};
    std::atomic<std::uint64_t> udpDatagrams{0};
    std::atomic<std::uint64_t> rawBytes{0};
    std::atomic<std::uint64_t> rawDatagrams{0};
  };

  mutable std::mutex mutex_;
  std::vector<FreqEntry> frequencies_;
  SignalSnapshot signal_;
  TelemetrySnapshot telemetry_;
  bool initialized_ = false;
  bool fatal_ = false;
  bool busy_ = false;
  bool monitoring_ = false;
  bool streaming_ = false;
  bool recording_ = false;
  std::string recordPath_;
  int currentMhz_ = 0;
  std::vector<MuxService> services_;
  ServiceSelectionSnapshot selectedService_;
  std::string phase_;
  std::string error_;
  Counters counters_;
  std::atomic<std::uint64_t> statusHits_{0};
  std::atomic<std::uint64_t> statusMisses_{0};
  std::atomic<double> rateMbps_{0.0};
  std::atomic<double> outputRateMbps_{0.0};
};

}  // namespace lme2510::tui
