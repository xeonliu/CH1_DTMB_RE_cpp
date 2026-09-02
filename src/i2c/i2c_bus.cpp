#include "lme2510/i2c/i2c_bus.hpp"

#include <sstream>

#include "lme2510/bridge/usb_bridge.hpp"
#include "lme2510/util/logger.hpp"

namespace lme2510 {
namespace {

std::string registerName(uint8_t devAddr, uint8_t reg) {
  if (devAddr == kTunerAddr) {
    const char* name = nullptr;
    switch (reg) {
      case 0x00: name = "N_div"; break;
      case 0x01: name = "K_frac_hi"; break;
      case 0x02: name = "K_frac_mid"; break;
      case 0x03: name = "K_frac_lo"; break;
      case 0x04: name = "track_filter"; break;
      case 0x05: name = "LNA"; break;
      case 0x06: name = "PLL_CFG"; break;
      case 0x07: name = "TEST"; break;
      case 0x08: name = "SHUTDOWN"; break;
      case 0x09: name = "VCO_CTRL"; break;
      case 0x0A: name = "BASEBAND"; break;
      case 0x0B: name = "DC_OFFSET_CTRL"; break;
      case 0x0C: name = "DC_OFFSET_DAC"; break;
      case 0x0D: name = "ROM_TABLE_ADDR"; break;
      case 0x10: name = "ROM_TABLE_DATA"; break;
      case 0x11: name = "STATUS"; break;
      case 0x12: name = "AUTOTUNE"; break;
      default: break;
    }
    std::ostringstream out;
    out << "TUNER reg 0x" << hexByte(reg);
    if (name != nullptr) {
      out << " [" << name << ']';
    }
    return out.str();
  }
  return std::string(reg >= 0xC0 ? "DEMODH" : "DEMOD") +
         " reg 0x" + hexByte(reg);
}

}  // namespace

BridgeI2cBus::BridgeI2cBus(UsbBridge& bridge, RegLogger* regLogger)
    : bridge_(bridge), regLogger_(regLogger) {}

bool BridgeI2cBus::writeSingle(uint8_t devAddr, uint8_t regAddr,
                               uint8_t value) {
  const bool ok = bridge_.writeSingle(devAddr, regAddr, value);
  if (regLogger_ != nullptr && regLogger_->enabled()) {
    regLogger_->log("I2C_WRITE", registerName(devAddr, regAddr) + " = 0x" +
                                     hexByte(value) +
                                     "  ack_ok=" + (ok ? "True" : "False"));
  }
  return ok;
}

std::optional<uint8_t> BridgeI2cBus::readSingle(uint8_t devAddr,
                                                uint8_t regAddr) {
  const std::optional<uint8_t> value = bridge_.readSingle(devAddr, regAddr);
  if (regLogger_ != nullptr && regLogger_->enabled()) {
    regLogger_->log("I2C_READ", registerName(devAddr, regAddr) + " -> " +
                                    (value ? "0x" + hexByte(*value)
                                           : "None"));
  }
  return value;
}

bool BridgeI2cBus::writeBlock(uint8_t devAddr, uint8_t regAddr,
                              const std::vector<uint8_t>& data) {
  const bool ok = bridge_.writeBlock(devAddr, regAddr, data);
  if (regLogger_ != nullptr && regLogger_->enabled()) {
    regLogger_->log("I2C_WRITE_BLOCK",
                    registerName(devAddr, regAddr) + "+ data=" +
                        hexBytes(data) +
                        "  ack_ok=" + (ok ? "True" : "False"));
  }
  return ok;
}

std::optional<std::vector<uint8_t>> BridgeI2cBus::readBlock(
    uint8_t devAddr, uint8_t regAddr, std::size_t count) {
  const auto value = bridge_.readBlock(devAddr, regAddr, count);
  if (regLogger_ != nullptr && regLogger_->enabled() &&
      devAddr == kTunerAddr) {
    regLogger_->log("I2C_READ_BLOCK",
                    registerName(devAddr, regAddr) + " n=" +
                        std::to_string(count) + " -> " +
                        (value ? hexBytes(*value) : "None"));
  }
  return value;
}

GatedI2cBus::GatedI2cBus(I2cBus& inner) : inner_(inner) {}

void GatedI2cBus::repeaterEnable() {
  inner_.writeSingle(kDemodAddr, 0x01, 0xE0);
}

void GatedI2cBus::repeaterDisable() {
  inner_.writeSingle(kDemodAddr, 0x01, 0x60);
}

bool GatedI2cBus::writeSingle(uint8_t devAddr, uint8_t regAddr,
                              uint8_t value) {
  repeaterEnable();
  try {
    const bool ok = inner_.writeSingle(devAddr, regAddr, value);
    repeaterDisable();
    return ok;
  } catch (...) {
    repeaterDisable();
    throw;
  }
}

std::optional<uint8_t> GatedI2cBus::readSingle(uint8_t devAddr,
                                               uint8_t regAddr) {
  repeaterEnable();
  try {
    const auto value = inner_.readSingle(devAddr, regAddr);
    repeaterDisable();
    return value;
  } catch (...) {
    repeaterDisable();
    throw;
  }
}

bool GatedI2cBus::writeBlock(uint8_t devAddr, uint8_t regAddr,
                             const std::vector<uint8_t>& data) {
  repeaterEnable();
  try {
    const bool ok = inner_.writeBlock(devAddr, regAddr, data);
    repeaterDisable();
    return ok;
  } catch (...) {
    repeaterDisable();
    throw;
  }
}

std::optional<std::vector<uint8_t>> GatedI2cBus::readBlock(
    uint8_t devAddr, uint8_t regAddr, std::size_t count) {
  repeaterEnable();
  try {
    const auto value = inner_.readBlock(devAddr, regAddr, count);
    repeaterDisable();
    return value;
  } catch (...) {
    repeaterDisable();
    throw;
  }
}

}  // namespace lme2510
