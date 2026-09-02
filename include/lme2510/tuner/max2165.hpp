#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lme2510/i2c/i2c_bus.hpp"

namespace lme2510 {

class RegLogger;

struct TunerCalibration {
  bool present = false;
  uint8_t lowBandGain = 0;
  uint8_t highBandGain = 0;
  uint8_t bwMin = 0;
  uint8_t bwMax = 0;
  uint8_t reg0aCal = 0;
};

struct TunerDividers {
  uint8_t n = 0;
  uint8_t kHi = 0;
  uint8_t kMid = 0;
  uint8_t kLo = 0;
  uint32_t k = 0;
};

/// MAX2165 tuner control: calibration readback, N/K divider arithmetic,
/// bandwidth/gain byte computation, init and frequency setting.  All register
/// traffic goes through the demodulator I2C repeater gate.
class Max2165Tuner {
 public:
  explicit Max2165Tuner(GatedI2cBus& bus);

  const TunerCalibration& readCalibration();
  TunerDividers calculateDividers(int freqMhz) const;
  uint8_t calculateBandwidthByte(int freqMhz,
                                 bool forceMaxGain = false) const;
  uint8_t calculateRegister0A() const;

  void init();
  void tune(int freqMhz);

  std::optional<uint8_t> readRegister(uint8_t reg);

 private:
  GatedI2cBus& bus_;
  TunerCalibration calibration_;
  uint8_t register0aLowNibble_ = 0x03;
};

}  // namespace lme2510
