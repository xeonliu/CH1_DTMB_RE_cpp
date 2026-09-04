#include "lme2510/tui/tui_engine.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "lme2510/util/logger.hpp"

namespace lme2510::tui {

namespace {

constexpr double kObserveSeconds = 2.2;

}  // namespace

TuiEngine::TuiEngine(const Options& options, RegLogger* regLogger,
                     std::ostream* statusLog, TuiModel& model)
    : options_(options),
      regLogger_(regLogger),
      statusLog_(statusLog),
      model_(model) {
  receiver_ = std::make_unique<Receiver>(makeReceiverOptions(options),
                                         regLogger, statusLog);
}

TuiEngine::~TuiEngine() {
  requestQuit();
  wait();
}

ReceiverOptions TuiEngine::makeReceiverOptions(const Options& options) {
  ReceiverOptions receiverOptions;
  receiverOptions.frequencyMhz = options.frequencyMhz;
  receiverOptions.pidList = options.pids;
  receiverOptions.pidMode = options.pidMode;
  receiverOptions.fw1Path = options.fw1Path;
  receiverOptions.fw2Path = options.fw2Path;
  return receiverOptions;
}

void TuiEngine::start() {
  worker_ = std::thread([this] { workerMain(); });
}

void TuiEngine::pushJob(Job job) {
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    if (job.kind == JobKind::kQuit) {
      quit_ = true;
    }
    jobs_.push_back(job);
  }
  jobCv_.notify_all();
}

void TuiEngine::requestScan(const std::vector<int>& checkedMhz) {
  Job job;
  job.kind = JobKind::kScan;
  job.mhzList = checkedMhz;
  pushJob(job);
}

void TuiEngine::requestTune(int mhz) {
  Job job;
  job.kind = JobKind::kTune;
  job.mhz = mhz;
  pushJob(job);
}

void TuiEngine::requestStream(bool enable) {
  Job job;
  job.kind = JobKind::kSetStream;
  job.enable = enable;
  pushJob(job);
}

void TuiEngine::requestRecord(bool enable) {
  Job job;
  job.kind = JobKind::kSetRecord;
  job.enable = enable;
  pushJob(job);
}

void TuiEngine::requestSelectService(std::uint16_t programNumber) {
  Job job;
  job.kind = JobKind::kSelectService;
  job.programNumber = programNumber;
  pushJob(job);
}

void TuiEngine::requestClearService() {
  Job job;
  job.kind = JobKind::kClearService;
  pushJob(job);
}

void TuiEngine::requestQuit() {
  Job job;
  job.kind = JobKind::kQuit;
  pushJob(job);
}

void TuiEngine::wait() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

void TuiEngine::workerMain() {
  model_.setPhase("初始化设备…");
  model_.setBusy(true);
  try {
    receiver_->initializeDevice();
    model_.setReady();
  } catch (const std::exception& error) {
    model_.setFatal(std::string(error.what()));
    model_.setBusy(false);
    return;
  }
  model_.setBusy(false);

  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(jobMutex_);
      jobCv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
      if (quit_) {
        break;
      }
      job = jobs_.front();
      jobs_.pop_front();
    }

    model_.setBusy(true);
    try {
      switch (job.kind) {
        case JobKind::kScan:
          scanFrequencies(job.mhzList);
          break;
        case JobKind::kTune:
          observeFrequency(job.mhz, true);
          break;
        case JobKind::kSetStream:
          if (capture_ != nullptr && capture_->running()) {
            capture_->setForwarding(job.enable);
            model_.setStreaming(job.enable);
            model_.setPhase(job.enable
                                ? "串流中"
                                : "监视中 (" +
                                      std::to_string(model_.currentMhz()) +
                                      " MHz)");
          }
          break;
        case JobKind::kSetRecord:
          if (capture_ != nullptr && capture_->running()) {
            capture_->setRecording(job.enable);
            model_.setRecording(job.enable);
            model_.setPhase(job.enable
                                ? "录制中"
                                : "监视中 (" +
                                      std::to_string(model_.currentMhz()) +
                                      " MHz)");
          }
          break;
        case JobKind::kSelectService: {
          const std::vector<MuxService> services = model_.services();
          for (const MuxService& service : services) {
            if (service.programNumber != job.programNumber) {
              continue;
            }
            ServiceSelectionSnapshot selection;
            selection.active = true;
            selection.programNumber = service.programNumber;
            selection.pmtPid = service.pmtPid;
            selection.name = service.name;
            selection.pids.push_back(service.pmtPid);
            for (const std::uint16_t pid : service.streamPids) {
              selection.pids.push_back(pid);
            }
            if (capture_ != nullptr && capture_->running()) {
              capture_->setSelectedService(
                  std::make_shared<const ServiceSelectionSnapshot>(selection));
            }
            model_.setSelectedService(selection);
            model_.setPhase("已选台: " + selection.name);
            break;
          }
          break;
        }
        case JobKind::kClearService:
          if (capture_ != nullptr && capture_->running()) {
            capture_->setSelectedService(nullptr);
          }
          model_.setSelectedService(ServiceSelectionSnapshot());
          model_.setPhase("整频点输出");
          break;
        case JobKind::kQuit:
          break;
      }
    } catch (const std::exception& error) {
      model_.setError(std::string("操作失败: ") + error.what());
      model_.setPhase("就绪");
    }
    model_.setBusy(false);
  }

  stopMonitorCapture();
}

void TuiEngine::scanFrequencies(const std::vector<int>& mhzList) {
  stopMonitorCapture();
  model_.setPhase("扫描准备…");
  for (const int mhz : mhzList) {
    observeFrequency(mhz, false);
  }
  model_.setPhase("扫描完成");
}

void TuiEngine::observeFrequency(int mhz, bool keepMonitoring) {
  stopMonitorCapture();
  model_.setPhase("调谐 " + std::to_string(mhz) + " MHz…");

  receiver_->tuneTo(mhz);
  receiver_->commitPidFilterDefaultAllPass();

  startMonitorCapture(mhz);

  FreqEntry result;
  result.mhz = mhz;
  result.current = keepMonitoring;
  result.scanned = true;

  const auto started = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       started)
             .count() < kObserveSeconds &&
         !quit_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const SignalSnapshot signal = model_.signal();
    const TsMetricsSnapshot counters = model_.counters();
    result.locked = receiver_->locked() ||
                    (signal.has && signal.lock);
    result.strengthPct = signal.has ? signal.strengthPct : 0;
    result.qualityPct = signal.has ? signal.qualityPct : 0;
    result.rateMbps = counters.rateMbps;
    result.packets = counters.packets;
    result.ccErrors = counters.ccErrors;
    result.ccLost = counters.ccLost;
    result.note = result.locked ? "LOCK" : "未锁定";
    model_.updateFrequencyResult(mhz, result);
    model_.setPhase("扫描 " + std::to_string(mhz) + " MHz… " +
                    result.note);
  }

  if (!keepMonitoring) {
    stopMonitorCapture();
  } else {
    model_.setPhase("监视中 (" + std::to_string(mhz) + " MHz)");
  }
}

void TuiEngine::startMonitorCapture(int mhz) {
  capture_ = std::make_unique<CaptureSession>(
      receiver_->usbBridge(), receiver_->demodulator(), &receiver_->tuner(),
      options_, statusLog_, regLogger_, model_);
  capture_->start();
  model_.setMonitoring(mhz, true);
  model_.setStreaming(false);
}

void TuiEngine::stopMonitorCapture() {
  if (capture_ != nullptr) {
    capture_->stop();
    capture_.reset();
  }
  model_.setMonitoring(0, false);
  model_.setStreaming(false);
  model_.setRecording(false);
  model_.clearServices();
}

}  // namespace lme2510::tui
