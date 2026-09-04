#pragma once

#include <condition_variable>
#include <deque>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lme2510/frontend/receiver.hpp"
#include "lme2510/tui/capture_session.hpp"
#include "lme2510/tui/tui_model.hpp"
#include "lme2510/util/arg_parser.hpp"

namespace lme2510 {

class RegLogger;

namespace tui {

/// Background device/session controller for the FTXUI TUI.  Owns all
/// long-running USB work so the UI thread only paints and reads input.
class TuiEngine {
 public:
  TuiEngine(const Options& options, RegLogger* regLogger,
            std::ostream* statusLog, TuiModel& model);
  ~TuiEngine();

  TuiEngine(const TuiEngine&) = delete;
  TuiEngine& operator=(const TuiEngine&) = delete;

  void start();
  void requestScan(const std::vector<int>& checkedMhz);
  void requestTune(int mhz);
  void requestStream(bool enable);
  void requestRecord(bool enable);
  void requestSelectService(std::uint16_t programNumber);
  void requestClearService();
  void requestQuit();
  void wait();

 private:
  enum class JobKind {
    kScan,
    kTune,
    kSetStream,
    kSetRecord,
    kSelectService,
    kClearService,
    kQuit
  };
  struct Job {
    JobKind kind = JobKind::kQuit;
    std::vector<int> mhzList;
    int mhz = 0;
    bool enable = false;
    std::uint16_t programNumber = 0;
  };

  void workerMain();
  void scanFrequencies(const std::vector<int>& mhzList);
  void observeFrequency(int mhz, bool keepMonitoring);
  void startMonitorCapture(int mhz);
  void stopMonitorCapture();
  void pushJob(Job job);

  static ReceiverOptions makeReceiverOptions(const Options& options);

  const Options& options_;
  RegLogger* regLogger_;
  std::ostream* statusLog_;
  TuiModel& model_;

  std::unique_ptr<Receiver> receiver_;
  std::unique_ptr<CaptureSession> capture_;

  std::mutex jobMutex_;
  std::condition_variable jobCv_;
  std::deque<Job> jobs_;
  bool quit_ = false;
  std::thread worker_;
};

}  // namespace tui
}  // namespace lme2510
