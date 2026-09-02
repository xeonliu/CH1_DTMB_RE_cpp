#include "lme2510/demod/demodulator.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

#include "lme2510/bridge/usb_bridge.hpp"
#include "lme2510/util/logger.hpp"
#include "lme2510/util/platform.hpp"

namespace lme2510 {

std::string demodChipName(DemodChip chip) {
  return chip == DemodChip::kLgs8Gl5 ? "LGS8GL5" : "LGS8G75";
}

int gl5StrengthPercent(bool lock, uint8_t signal, uint8_t hi) {
  const int low3 = signal & 0x07;
  const uint16_t word =
      static_cast<uint16_t>(signal | (static_cast<uint16_t>(hi) << 8));
  if (!lock) {
    return low3 + 8;
  }
  if (word >= 0x1F00 && word <= 0x1FFF) {
    return low3 + 80;
  }
  if (word >= 0x0058 && word <= 0x00FF) {
    return low3 + 70;
  }
  if (word >= 0x0100 && word <= 0x0180) {
    return low3 + 60;
  }
  if (word >= 0x0180 && word <= 0x0200) {
    return low3 + 50;
  }
  return 69;
}

int gl5QualityPercent(bool lock, uint8_t snrRaw) {
  const int mod = snrRaw % 15;
  const int linear = (100 * static_cast<int>(snrRaw)) / 255;
  if (!lock) {
    return mod + 5;
  }
  if (linear == 0) {
    return mod + 30;
  }
  if (linear <= 40) {
    return 90 - linear;
  }
  if (linear < 90) {
    return 100 - ((3 * linear / 4) % 100);
  }
  return 30;
}

std::optional<Ep8aStatus> parseFirstStatusPacket(
    const std::vector<uint8_t>& raw) {
  for (std::size_t offset = 0; offset + 8 <= raw.size(); offset += 8) {
    if (raw[offset] != 0xBB) {
      continue;
    }
    Ep8aStatus status;
    status.type = raw[offset + 1];
    status.lock = raw[offset + 2] != 0;
    status.signalLevel = raw[offset + 3];
    status.snrRaw = raw[offset + 4];
    status.hi = raw[offset + 5];
    status.lo = raw[offset + 6];
    status.raw.assign(raw.begin() + static_cast<std::ptrdiff_t>(offset),
                      raw.begin() + static_cast<std::ptrdiff_t>(offset + 8));
    status.strengthPercent =
        gl5StrengthPercent(status.lock, status.signalLevel, status.hi);
    status.qualityPercent = gl5QualityPercent(status.lock, status.snrRaw);
    return status;
  }
  return std::nullopt;
}

std::string ep8aStatusLine(const Ep8aStatus& status) {
  std::ostringstream out;
  out << "EP8A raw=" << hexBytes(status.raw)
      << " lock=" << static_cast<int>(status.lock)
      << " signal=0x" << hexByte(status.signalLevel)
      << " snr_raw=0x" << hexByte(status.snrRaw)
      << " hi=0x" << hexByte(status.hi)
      << " lo=0x" << hexByte(status.lo)
      << " strength=" << status.strengthPercent
      << "% quality=" << status.qualityPercent << '%';
  return out.str();
}

Demodulator::Demodulator(I2cBus& bus, DemodChip chip)
    : bus_(bus), chip_(chip), chipName_(demodChipName(chip)) {}

DemodChip Demodulator::identify(I2cBus& bus, int retries,
                                double retryDelaySeconds) {
  for (int attempt = 0; attempt < retries; ++attempt) {
    const std::optional<uint8_t> value = bus.readSingle(kDemodAddr, 0x00);
    if (value) {
      const DemodChip chip = *value == 0x0E ? DemodChip::kLgs8Gl5
                                            : DemodChip::kLgs8G75;
      std::cout << "  Demod reg[0x00] = 0x" << hexByte(*value) << "  ->  "
                << demodChipName(chip) << '\n';
      return chip;
    }
    if (attempt + 1 < retries) {
      sleepSeconds(retryDelaySeconds);
    }
  }
  throw std::runtime_error(
      "Cannot read Demod reg 0x00 after retries - check USB connection and "
      "that firmware is loaded");
}

uint8_t DemodulatorPhysicalAddress(uint8_t reg) {
  return reg >= 0xC0 ? kDemodHighAddr : kDemodAddr;
}

bool Demodulator::writeRegister(uint8_t reg, uint8_t value) {
  return bus_.writeSingle(DemodulatorPhysicalAddress(reg), reg, value);
}

std::optional<uint8_t> Demodulator::readRegister(uint8_t reg) {
  return bus_.readSingle(DemodulatorPhysicalAddress(reg), reg);
}

bool Demodulator::initAfterIdentify() {
  const std::optional<uint8_t> reg7 = readRegister(0x07);
  if (!reg7) {
    return true;
  }
  writeRegister(0x07, static_cast<uint8_t>(*reg7 | 0x0C));
  for (const uint8_t reg : {0x09, 0x0A, 0x0B, 0x0C}) {
    writeRegister(reg, 0x00);
  }
  const std::optional<uint8_t> reg7b = readRegister(0x07);
  if (reg7b) {
    writeRegister(0x07, static_cast<uint8_t>(*reg7b & 0x7C));
  }
  return true;
}

bool Demodulator::initAfterTune() {
  const std::optional<uint8_t> reg7 = readRegister(0x07);
  if (reg7) {
    writeRegister(0x07, static_cast<uint8_t>(*reg7 | 0x0C));
  }
  for (const uint8_t reg : {0x08, 0x09, 0x0A, 0x0B}) {
    writeRegister(reg, 0x00);
  }

  const std::optional<uint8_t> reg7b = readRegister(0x07);
  if (reg7b) {
    writeRegister(0x07, static_cast<uint8_t>(*reg7b & 0x7F));
  }

  const std::optional<uint8_t> regC = readRegister(0x0C);
  if (regC) {
    writeRegister(0x0C, static_cast<uint8_t>((*regC & 0x7B) | 0x80));
  }
  writeRegister(0x39, 0x00);
  writeRegister(0x3D, 0x04);
  return true;
}

bool Demodulator::softReset() {
  if (!readRegister(0x00)) {
    return false;
  }
  const std::optional<uint8_t> reg2 = readRegister(0x02);
  if (!reg2) {
    return false;
  }
  if (!writeRegister(0x02, static_cast<uint8_t>(*reg2 & 0xFE))) {
    return false;
  }
  if (!writeRegister(0x02, static_cast<uint8_t>(*reg2 | 0x01))) {
    return false;
  }
  sleepMilliseconds(5);
  return true;
}

bool Demodulator::prepareForLock() {
  const std::optional<uint8_t> reg3 = readRegister(0x03);
  if (!reg3 || !writeRegister(0x03, static_cast<uint8_t>(*reg3 & 0xFE))) {
    return false;
  }
  const std::optional<uint8_t> reg7e = readRegister(0x7E);
  if (!reg7e || !writeRegister(0x7E, static_cast<uint8_t>(*reg7e | 0x01))) {
    return false;
  }
  const std::optional<uint8_t> regC5 = readRegister(0xC5);
  if (!regC5 || !writeRegister(0xC5, static_cast<uint8_t>(*regC5 & 0xE0))) {
    return false;
  }
  return true;
}

bool Demodulator::pollLockRegister(double timeoutSeconds,
                                   double intervalSeconds) {
  std::cout << "\nPolling lock via reg 0x4B (timeout " << timeoutSeconds
            << "s)...\n";
  const double end = 0.0 + timeoutSeconds;
  double elapsed = 0.0;
  while (elapsed < end) {
    const std::optional<uint8_t> value = readRegister(0x4B);
    if (value) {
      const bool locked = (*value & 0x01) != 0;
      std::cout << "  reg[0x4B] = 0x" << hexByte(*value) << "  "
                << (locked ? "LOCKED" : "unlocked") << '\n';
      if (locked) {
        return true;
      }
    }
    sleepSeconds(intervalSeconds);
    elapsed += intervalSeconds;
  }
  std::cout << "  Timed out - no lock.\n";
  return false;
}

namespace {

bool pollDemodMaskEquals(Demodulator& demod, uint8_t reg, uint8_t mask,
                         uint8_t expected, int attempts, double interval) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const std::optional<uint8_t> value = demod.readRegister(reg);
    if (value) {
      const bool ok = ((*value & mask) == expected);
      std::cout << "  reg[0x" << hexByte(reg) << "] = 0x" << hexByte(*value)
                << "  mask 0x" << hexByte(mask) << " -> "
                << (ok ? "ok" : "wait") << '\n';
      if (ok) {
        return true;
      }
    }
    sleepSeconds(interval);
  }
  return false;
}

}  // namespace

Lgs8Gl5::Lgs8Gl5(I2cBus& bus, DemodChip chip) : Demodulator(bus, chip) {}

namespace {

bool setLgs8gl5RegC5LowbitsAndReset(Lgs8Gl5& demod) {
  const std::optional<uint8_t> reg7e = demod.readRegister(0x7E);
  if (!reg7e || !demod.writeRegister(0x7E, static_cast<uint8_t>(*reg7e & 0xFE))) {
    return false;
  }
  const std::optional<uint8_t> regC5 = demod.readRegister(0xC5);
  if (!regC5 ||
      !demod.writeRegister(0xC5,
                           static_cast<uint8_t>((*regC5 & 0xE0) | 0x06))) {
    return false;
  }
  return demod.softReset();
}

bool writeReg7dAndReset(Lgs8Gl5& demod, uint8_t value) {
  if (!demod.writeRegister(0x7D, value)) {
    return false;
  }
  return demod.softReset();
}

bool trainLgs8Gl5Lock(Lgs8Gl5& demod, int maxRounds = 5) {
  std::cout << "\n[LGS8GL5 lock training]\n";
  for (int roundIndex = 0; roundIndex < maxRounds; ++roundIndex) {
    const int mode = roundIndex % 5;
    if (mode == 1 || mode == 3) {
      continue;
    }

    const std::optional<uint8_t> reg4 = demod.readRegister(0x04);
    const std::optional<uint8_t> reg37 = demod.readRegister(0x37);
    if (!reg4 || !reg37) {
      std::cout << "  training aborted: could not read reg 0x04/0x37\n";
      return false;
    }

    uint8_t new4 = static_cast<uint8_t>(*reg4 & 0xFC);
    uint8_t new37 = *reg37;
    if (mode == 0) {
      new4 |= 0x02;
      new37 &= 0x7F;
    } else if (mode == 2) {
      new37 &= 0x7F;
    } else if (mode == 4) {
      new4 |= 0x01;
      new37 |= 0x80;
    }

    std::cout << "  round " << (roundIndex + 1) << ": mode=" << mode
              << "  reg04 0x" << hexByte(*reg4) << "->0x" << hexByte(new4)
              << "  reg37 0x" << hexByte(*reg37) << "->0x" << hexByte(new37)
              << '\n';
    if (!demod.writeRegister(0x04, new4) ||
        !demod.writeRegister(0x37, new37)) {
      return false;
    }
    if (!demod.softReset()) {
      return false;
    }

    if (pollDemodMaskEquals(demod, 0x4B, 0x80, 0x80, 30, 0.02) &&
        pollDemodMaskEquals(demod, 0xA4, 0x03, 0x01, 20, 0.01)) {
      const std::optional<uint8_t> regA2 = demod.readRegister(0xA2);
      if (!regA2) {
        return false;
      }
      if (!setLgs8gl5RegC5LowbitsAndReset(demod) ||
          !writeReg7dAndReset(demod, *regA2)) {
        return false;
      }
      std::cout << "  LGS8GL5 lock training: LOCKED\n";
      return true;
    }

    const std::optional<uint8_t> reg7c = demod.readRegister(0x7C);
    if (reg7c) {
      demod.writeRegister(0x7C, static_cast<uint8_t>(*reg7c ^ 0x80));
    }
  }
  std::cout << "  LGS8GL5 lock training: no lock\n";
  return false;
}

}  // namespace

bool Lgs8Gl5::lockAfterTune() {
  if (!prepareForLock()) {
    std::cout << "  Warning: LGS8GL5 lock preparation failed\n";
  }
  softReset();
  const bool locked = trainLgs8Gl5Lock(*this);
  if (locked) {
    return true;
  }
  std::cout << "\nFallback: polling simple reg 0x4B bit0 lock...\n";
  return pollLockRegister(5.0, 0.1);
}

Lgs8G75::Lgs8G75(I2cBus& bus, DemodChip chip) : Demodulator(bus, chip) {}

bool Lgs8G75::lockAfterTune() {
  initAfterTune();
  return pollLockRegister(5.0, 0.1);
}

}  // namespace lme2510
