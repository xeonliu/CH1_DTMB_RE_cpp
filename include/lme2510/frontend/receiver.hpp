#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "lme2510/bridge/usb_bridge.hpp"
#include "lme2510/demod/demodulator.hpp"
#include "lme2510/i2c/i2c_bus.hpp"
#include "lme2510/tuner/max2165.hpp"

namespace lme2510 {

class RegLogger;

struct ReceiverOptions {
  int frequencyMhz = 618;
  std::string pidList;       // empty => default all-pass 0x1FFF commit
  int pidMode = 0;           // 0 = allow-list; 2 = clear/advanced
  std::string fw1Path;       // empty => embedded stage-1 blob
  std::string fw2Path;       // empty => embedded stage-2 LGS8G75 blob
};

/// Orchestrates the complete device bring-up:
/// open/firmware → identify → CMD 0x16 → tuner init → post-identify init →
/// tune → lock → PID-filter commit → one status sample.
class Receiver {
 public:
  Receiver(const ReceiverOptions& options, RegLogger* regLogger,
           std::ostream* statusLog);
  ~Receiver();

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  /// Full configuration phase.  Returns the final lock status.
  bool runConfiguration();

  UsbBridge& usbBridge() { return bridge_; }
  Max2165Tuner& tuner() { return tuner_; }
  Demodulator* demodulator() { return demod_.get(); }
  bool locked() const { return locked_; }
  DemodChip chip() const { return chip_; }

 private:
  void openLogged();
  void reopenAfterFirmwareDownload();
  void commitPidFilterAndSampleStatus();
  void initializeAndTune();

  ReceiverOptions options_;
  RegLogger* regLogger_;
  std::ostream* statusLog_;

  UsbBridge bridge_;
  BridgeI2cBus rawI2c_;
  GatedI2cBus gatedI2c_;
  Max2165Tuner tuner_;
  std::unique_ptr<Demodulator> demod_;
  DemodChip chip_ = DemodChip::kLgs8Gl5;
  bool locked_ = false;
};

}  // namespace lme2510
