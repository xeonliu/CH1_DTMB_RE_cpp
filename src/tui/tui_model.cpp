#include "lme2510/tui/tui_model.hpp"

#include <algorithm>
#include <cmath>

namespace lme2510::tui {

namespace {

constexpr double kLossCapPercent = 100.0;

}  // namespace

TuiModel::TuiModel() = default;

void TuiModel::replaceFrequencies(const std::vector<int>& mhzList) {
  std::lock_guard<std::mutex> lock(mutex_);
  frequencies_.clear();
  frequencies_.reserve(mhzList.size());
  for (const int mhz : mhzList) {
    FreqEntry entry;
    entry.mhz = mhz;
    frequencies_.push_back(entry);
  }
}

std::vector<FreqEntry> TuiModel::frequencies() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frequencies_;
}

void TuiModel::setChecked(std::size_t index, bool checked) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index < frequencies_.size()) {
    frequencies_[index].checked = checked;
  }
}

void TuiModel::updateFrequencyResult(int mhz, const FreqEntry& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (FreqEntry& entry : frequencies_) {
    if (entry.mhz != mhz) {
      continue;
    }
    entry.scanned = result.scanned;
    entry.locked = result.locked;
    entry.strengthPct = result.strengthPct;
    entry.qualityPct = result.qualityPct;
    entry.rateMbps = result.rateMbps;
    entry.packets = result.packets;
    entry.ccErrors = result.ccErrors;
    entry.ccLost = result.ccLost;
    entry.note = result.note;
    entry.current = result.current;
    return;
  }
}

void TuiModel::setPhase(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  phase_ = text;
}

void TuiModel::setBusy(bool busy) {
  std::lock_guard<std::mutex> lock(mutex_);
  busy_ = busy;
}

void TuiModel::setReady() {
  std::lock_guard<std::mutex> lock(mutex_);
  initialized_ = true;
  phase_ = "设备就绪";
}

void TuiModel::setFatal(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  initialized_ = false;
  fatal_ = true;
  busy_ = false;
  monitoring_ = false;
  streaming_ = false;
  recording_ = false;
  phase_ = "错误";
  error_ = text;
}

void TuiModel::setMonitoring(int mhz, bool monitoring) {
  std::lock_guard<std::mutex> lock(mutex_);
  monitoring_ = monitoring;
  currentMhz_ = monitoring ? mhz : currentMhz_;
}

void TuiModel::setStreaming(bool streaming) {
  std::lock_guard<std::mutex> lock(mutex_);
  streaming_ = streaming;
  if (phase_ != "错误") {
    phase_ = streaming ? "串流中" : (recording_ ? "录制中" : "监视中");
  }
}

void TuiModel::setRecording(bool recording) {
  std::lock_guard<std::mutex> lock(mutex_);
  recording_ = recording;
  if (phase_ != "错误") {
    phase_ = recording ? "录制中" : (streaming_ ? "串流中" : "监视中");
  }
}

void TuiModel::setRecordPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  recordPath_ = path;
}

std::string TuiModel::recordPath() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recordPath_;
}

void TuiModel::setServices(const std::vector<MuxService>& services) {
  std::lock_guard<std::mutex> lock(mutex_);
  services_ = services;
}

std::vector<MuxService> TuiModel::services() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return services_;
}

void TuiModel::clearServices() {
  std::lock_guard<std::mutex> lock(mutex_);
  services_.clear();
  selectedService_ = ServiceSelectionSnapshot();
}

void TuiModel::setSelectedService(const ServiceSelectionSnapshot& service) {
  std::lock_guard<std::mutex> lock(mutex_);
  selectedService_ = service;
}

ServiceSelectionSnapshot TuiModel::selectedService() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selectedService_;
}

void TuiModel::setError(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  error_ = text;
  if (!fatal_) {
    phase_ = "监视中";
  }
}

void TuiModel::setSignal(const SignalSnapshot& signal) {
  std::lock_guard<std::mutex> lock(mutex_);
  signal_ = signal;
}

SignalSnapshot TuiModel::signal() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return signal_;
}

void TuiModel::resetSignal() {
  std::lock_guard<std::mutex> lock(mutex_);
  signal_ = SignalSnapshot();
}

void TuiModel::setTelemetry(const TelemetrySnapshot& telemetry) {
  std::lock_guard<std::mutex> lock(mutex_);
  telemetry_ = telemetry;
}

void TuiModel::clearTelemetry() {
  std::lock_guard<std::mutex> lock(mutex_);
  telemetry_ = TelemetrySnapshot();
}

TelemetrySnapshot TuiModel::telemetry() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return telemetry_;
}

void TuiModel::setRateMbps(double rate) {
  rateMbps_.store(rate);
}

void TuiModel::setOutputRateMbps(double rate) {
  outputRateMbps_.store(rate);
}

void TuiModel::addBytes(std::uint64_t bytes) {
  counters_.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void TuiModel::addPackets(std::uint64_t packets) {
  counters_.packets.fetch_add(packets, std::memory_order_relaxed);
}

void TuiModel::addFrames(std::uint64_t frames) {
  counters_.frames.fetch_add(frames, std::memory_order_relaxed);
}

void TuiModel::addTimeout() {
  counters_.timeouts.fetch_add(1, std::memory_order_relaxed);
}

void TuiModel::addResync(std::uint64_t count) {
  counters_.resyncs.fetch_add(count, std::memory_order_relaxed);
}

void TuiModel::addDroppedBytes(std::uint64_t bytes) {
  counters_.droppedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void TuiModel::addCcLoss(std::uint64_t errors, std::uint64_t lost,
                         std::uint64_t transportErrors) {
  counters_.ccErrors.fetch_add(errors, std::memory_order_relaxed);
  counters_.ccLost.fetch_add(lost, std::memory_order_relaxed);
  counters_.transportErrors.fetch_add(transportErrors,
                                      std::memory_order_relaxed);
}

void TuiModel::addUdp(std::uint64_t bytes, std::uint64_t datagrams) {
  counters_.udpBytes.fetch_add(bytes, std::memory_order_relaxed);
  counters_.udpDatagrams.fetch_add(datagrams, std::memory_order_relaxed);
}

void TuiModel::addRaw(std::uint64_t bytes, std::uint64_t datagrams) {
  counters_.rawBytes.fetch_add(bytes, std::memory_order_relaxed);
  counters_.rawDatagrams.fetch_add(datagrams, std::memory_order_relaxed);
}

void TuiModel::addStatusSample(bool hit) {
  if (hit) {
    statusHits_.fetch_add(1, std::memory_order_relaxed);
  } else {
    statusMisses_.fetch_add(1, std::memory_order_relaxed);
  }
}

void TuiModel::resetCounters() {
  counters_.bytes.store(0);
  counters_.packets.store(0);
  counters_.frames.store(0);
  counters_.timeouts.store(0);
  counters_.resyncs.store(0);
  counters_.droppedBytes.store(0);
  counters_.ccErrors.store(0);
  counters_.ccLost.store(0);
  counters_.transportErrors.store(0);
  counters_.udpBytes.store(0);
  counters_.udpDatagrams.store(0);
  counters_.rawBytes.store(0);
  counters_.rawDatagrams.store(0);
  statusHits_.store(0);
  statusMisses_.store(0);
  rateMbps_.store(0.0);
  outputRateMbps_.store(0.0);
}

TsMetricsSnapshot TuiModel::counters() const {
  TsMetricsSnapshot out;
  out.bytes = counters_.bytes.load(std::memory_order_relaxed);
  out.packets = counters_.packets.load(std::memory_order_relaxed);
  out.frames = counters_.frames.load(std::memory_order_relaxed);
  out.timeouts = counters_.timeouts.load(std::memory_order_relaxed);
  out.resyncs = counters_.resyncs.load(std::memory_order_relaxed);
  out.droppedBytes = counters_.droppedBytes.load(std::memory_order_relaxed);
  out.ccErrors = counters_.ccErrors.load(std::memory_order_relaxed);
  out.ccLost = counters_.ccLost.load(std::memory_order_relaxed);
  out.transportErrors =
      counters_.transportErrors.load(std::memory_order_relaxed);
  out.udpBytes = counters_.udpBytes.load(std::memory_order_relaxed);
  out.udpDatagrams = counters_.udpDatagrams.load(std::memory_order_relaxed);
  out.rawBytes = counters_.rawBytes.load(std::memory_order_relaxed);
  out.rawDatagrams = counters_.rawDatagrams.load(std::memory_order_relaxed);
  out.rateMbps = rateMbps_.load();
  out.outputRateMbps = outputRateMbps_.load();

  const std::uint64_t received = out.packets;
  const std::uint64_t estimatedLost =
      out.ccLost + out.droppedBytes / static_cast<std::uint64_t>(188);
  const std::uint64_t denominator = received + estimatedLost;
  out.lossPct = denominator == 0
                    ? 0.0
                    : std::min(kLossCapPercent,
                               100.0 * static_cast<double>(estimatedLost) /
                                   static_cast<double>(denominator));
  return out;
}

}  // namespace lme2510::tui
