#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lme2510/i2c/i2c_bus.hpp"

namespace lme2510 {

enum class DemodChip {
  kLgs8Gl5,
  kLgs8G75,
};

std::string demodChipName(DemodChip chip);

struct Ep8aStatus {
  bool lock = false;
  uint8_t signalLevel = 0;
  uint8_t snrRaw = 0;
  uint8_t hi = 0;
  uint8_t lo = 0;
  uint8_t type = 0;
  std::vector<uint8_t> raw;
  int strengthPercent = 0;
  int qualityPercent = 0;
};

// LGS8GL5 percent conversions from LME2510_Analysis.md §5.5.
int gl5StrengthPercent(bool lock, uint8_t signal, uint8_t hi);
int gl5QualityPercent(bool lock, uint8_t snrRaw);

/// Parses the first complete 8-byte 0xBB status packet in a raw interrupt
/// transfer, decodes the GL5 percent values, and returns it.
std::optional<Ep8aStatus> parseFirstStatusPacket(
    const std::vector<uint8_t>& raw);

std::string ep8aStatusLine(const Ep8aStatus& status);

/// Demodulator base class: handles logical register routing (0x32 / 0x36),
/// identification, soft reset, post-identify init and register telemetry.
/// Lock acquisition is chip-specific and delegated to subclasses.
class Demodulator {
 public:
  Demodulator(I2cBus& bus, DemodChip chip);
  virtual ~Demodulator() = default;

  Demodulator(const Demodulator&) = delete;
  Demodulator& operator=(const Demodulator&) = delete;

  /// Reads Demod reg 0x00 up to *retries* times and classifies the chip.
  /// Throws UsbError-like std::runtime_error if no value is obtained.
  static DemodChip identify(I2cBus& bus, int retries = 5,
                            double retryDelaySeconds = 0.5);

  virtual bool initAfterIdentify();
  virtual bool lockAfterTune() = 0;
  bool softReset();

  /// Single-register access through the physical 0x32/0x36 routing.
  bool writeRegister(uint8_t reg, uint8_t value);
  std::optional<uint8_t> readRegister(uint8_t reg);

  DemodChip chip() const { return chip_; }
  const std::string& chipName() const { return chipName_; }

  bool pollLockRegister(double timeoutSeconds = 5.0,
                        double intervalSeconds = 0.1);

 protected:
  bool initAfterTune();
  bool prepareForLock();

  I2cBus& bus_;
  DemodChip chip_;
  std::string chipName_;
};

class Lgs8Gl5 : public Demodulator {
 public:
  Lgs8Gl5(I2cBus& bus, DemodChip chip);
  bool lockAfterTune() override;
};

class Lgs8G75 : public Demodulator {
 public:
  Lgs8G75(I2cBus& bus, DemodChip chip);
  bool lockAfterTune() override;
};

}  // namespace lme2510
