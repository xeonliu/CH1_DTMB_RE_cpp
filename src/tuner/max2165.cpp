#include "lme2510/tuner/max2165.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "lme2510/bridge/usb_bridge.hpp"
#include "lme2510/util/logger.hpp"

namespace lme2510 {

Max2165Tuner::Max2165Tuner(GatedI2cBus& bus) : bus_(bus) {}

TunerDividers Max2165Tuner::calculateDividers(int freqMhz) const {
  TunerDividers result;
  result.n = static_cast<uint8_t>(freqMhz / kRefFreqMhz);
  result.k = static_cast<uint32_t>(((freqMhz % kRefFreqMhz) << 20) /
                                   kRefFreqMhz);
  result.kHi = static_cast<uint8_t>(0x10 | ((result.k >> 16) & 0x0F));
  result.kMid = static_cast<uint8_t>((result.k >> 8) & 0xFF);
  result.kLo = static_cast<uint8_t>(result.k & 0xFF);
  return result;
}

uint8_t Max2165Tuner::calculateBandwidthByte(int freqMhz,
                                             bool forceMaxGain) const {
  int gain = 0xF;
  int bw = 0;
  if (!forceMaxGain && calibration_.present) {
    const int bwMin = calibration_.bwMin;
    const int bwMax = calibration_.bwMax;
    gain = freqMhz >= 725 ? calibration_.lowBandGain
                          : calibration_.highBandGain;
    bw = bwMin + (freqMhz - 470) * (bwMax - bwMin) / 310;
  }
  bw = std::max(0, std::min(15, bw));
  return static_cast<uint8_t>((bw & 0x0F) | ((gain & 0x0F) << 4));
}

uint8_t Max2165Tuner::calculateRegister0A() const {
  int hi = 0;
  if (calibration_.present) {
    hi = std::max(0, std::min(15, static_cast<int>(calibration_.reg0aCal) - 2));
  }
  return static_cast<uint8_t>((hi << 4) | (register0aLowNibble_ & 0x0F));
}

const TunerCalibration& Max2165Tuner::readCalibration() {
  std::vector<uint8_t> reads;
  reads.reserve(5);
  for (uint8_t i = 1; i <= 5; ++i) {
    bus_.writeBlock(kTunerAddr, 0x0D, {i});
    const std::optional<uint8_t> value = bus_.readSingle(kTunerAddr, 0x10);
    reads.push_back(value ? *value : 0);
  }
  bus_.writeBlock(kTunerAddr, 0x0D, {0});

  const uint8_t v3 = reads[0];
  const uint8_t v4 = reads[1];
  const uint8_t v5 = reads[2];
  calibration_.present = true;
  calibration_.lowBandGain = static_cast<uint8_t>(v3 & 0x0F);
  calibration_.highBandGain = static_cast<uint8_t>(v3 >> 4);
  calibration_.bwMin = static_cast<uint8_t>(v4 & 0x0F);
  calibration_.bwMax = static_cast<uint8_t>(v4 >> 4);
  calibration_.reg0aCal = static_cast<uint8_t>(v5 >> 4);

  std::cout << "  Calibration: low_band_gain="
            << static_cast<int>(calibration_.lowBandGain)
            << " high_band_gain="
            << static_cast<int>(calibration_.highBandGain)
            << " bw_min=" << static_cast<int>(calibration_.bwMin)
            << " bw_max=" << static_cast<int>(calibration_.bwMax)
            << " reg_0a_cal=" << static_cast<int>(calibration_.reg0aCal)
            << '\n';
  return calibration_;
}

void Max2165Tuner::init() {
  constexpr int kBaseFrequencyMhz = 474;
  std::cout << "  Initializing tuner (MAX2165)...\n";

  readCalibration();
  const TunerDividers dividers = calculateDividers(kBaseFrequencyMhz);
  const uint8_t bw = calculateBandwidthByte(kBaseFrequencyMhz, true);
  const uint8_t reg0a = calculateRegister0A();

  std::vector<uint8_t> initRegs = {
      dividers.n,
      dividers.kHi,
      dividers.kMid,
      dividers.kLo,
      bw,     // reg 0x04: BW/Gain
      0x01,   // reg 0x05
      0x0A,   // reg 0x06
      0x08,   // reg 0x07
      0x02,   // reg 0x08
      0x54,   // reg 0x09
      reg0a,  // reg 0x0A
      0x75,   // reg 0x0B
      0x00,   // reg 0x0C
      0x00,   // reg 0x0D
      0x00,   // reg 0x0E
  };
  if (initRegs.size() != 15) {
    throw std::runtime_error("internal error: MAX2165 init table size");
  }

  if (!bus_.writeBlock(kTunerAddr, 0x00, initRegs)) {
    throw std::runtime_error("Tuner init block write failed");
  }
  std::cout << "  15 regs written (base=" << kBaseFrequencyMhz
            << " MHz, BW=0x" << hexByte(bw) << ", reg0A=0x" << hexByte(reg0a)
            << ")\n";
}

void Max2165Tuner::tune(int freqMhz) {
  std::cout << '\n';
  for (int i = 0; i < 50; ++i) {
    std::cout << '-';
  }
  std::cout << '\n' << "Tuning to " << freqMhz << " MHz\n";
  for (int i = 0; i < 50; ++i) {
    std::cout << '-';
  }
  std::cout << '\n';

  const TunerDividers dividers = calculateDividers(freqMhz);
  const uint8_t bw = calculateBandwidthByte(freqMhz);
  const uint8_t reg0a = calculateRegister0A();

  std::cout << "  N=0x" << hexByte(dividers.n) << " (" << static_cast<int>(dividers.n)
            << ")  K=0x" << std::uppercase << std::hex << std::setfill('0')
            << std::setw(8) << dividers.k << std::dec << std::setfill(' ')
            << "  BW=0x" << hexByte(bw) << "  reg0A=0x" << hexByte(reg0a)
            << '\n';

  const std::vector<uint8_t> nkBytes = {dividers.n, dividers.kHi,
                                        dividers.kMid, dividers.kLo, bw};
  if (!bus_.writeBlock(kTunerAddr, 0x00, nkBytes)) {
    throw std::runtime_error("tuner N/K/BW write failed");
  }
  if (!bus_.writeBlock(kTunerAddr, 0x0A, {reg0a})) {
    throw std::runtime_error("tuner reg 0x0A write failed");
  }

  const std::optional<std::vector<uint8_t>> value =
      bus_.readBlock(kTunerAddr, 0x04, 1);
  if (value && !value->empty()) {
    const uint8_t new4 = static_cast<uint8_t>((*value)[0] | 0xF0);
    bus_.writeBlock(kTunerAddr, 0x04, {new4});
    std::cout << "  PLL latch: reg[0x04] 0x" << hexByte((*value)[0])
              << " -> 0x" << hexByte(new4) << '\n';
  } else {
    std::cout << "  Warning: could not read reg[0x04] for PLL latch\n";
  }

  std::cout << "  Tune complete.\n";
}

std::optional<uint8_t> Max2165Tuner::readRegister(uint8_t reg) {
  return bus_.readSingle(kTunerAddr, reg);
}

}  // namespace lme2510
