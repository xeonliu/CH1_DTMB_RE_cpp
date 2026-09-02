#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace lme2510 {

class RegLogger;
class UsbBridge;

/// Register-oriented I2C abstraction.  Devices see only byte/block reads and
/// writes; address routing and register purpose stay with the upper layers.
class I2cBus {
 public:
  virtual ~I2cBus() = default;

  virtual bool writeSingle(uint8_t devAddr, uint8_t regAddr, uint8_t value) = 0;
  virtual std::optional<uint8_t> readSingle(uint8_t devAddr,
                                            uint8_t regAddr) = 0;
  virtual bool writeBlock(uint8_t devAddr, uint8_t regAddr,
                          const std::vector<uint8_t>& data) = 0;
  virtual std::optional<std::vector<uint8_t>> readBlock(uint8_t devAddr,
                                                        uint8_t regAddr,
                                                        std::size_t count) = 0;
};

/// Direct bridge-backed I2C bus.  Logs every register operation in the same
/// way the Python LoggingLME2510 wrapper did.
class BridgeI2cBus : public I2cBus {
 public:
  BridgeI2cBus(UsbBridge& bridge, RegLogger* regLogger);

  bool writeSingle(uint8_t devAddr, uint8_t regAddr, uint8_t value) override;
  std::optional<uint8_t> readSingle(uint8_t devAddr, uint8_t regAddr) override;
  bool writeBlock(uint8_t devAddr, uint8_t regAddr,
                  const std::vector<uint8_t>& data) override;
  std::optional<std::vector<uint8_t>> readBlock(uint8_t devAddr,
                                                uint8_t regAddr,
                                                std::size_t count) override;

 private:
  UsbBridge& bridge_;
  RegLogger* regLogger_;
};

/// Wraps an I2cBus with the LGS demod I2C repeater open/close dance required
/// for every MAX2165 transaction.
class GatedI2cBus : public I2cBus {
 public:
  explicit GatedI2cBus(I2cBus& inner);

  bool writeSingle(uint8_t devAddr, uint8_t regAddr, uint8_t value) override;
  std::optional<uint8_t> readSingle(uint8_t devAddr, uint8_t regAddr) override;
  bool writeBlock(uint8_t devAddr, uint8_t regAddr,
                  const std::vector<uint8_t>& data) override;
  std::optional<std::vector<uint8_t>> readBlock(uint8_t devAddr,
                                                uint8_t regAddr,
                                                std::size_t count) override;

 private:
  I2cBus& inner_;
  void repeaterEnable();
  void repeaterDisable();
};

}  // namespace lme2510
