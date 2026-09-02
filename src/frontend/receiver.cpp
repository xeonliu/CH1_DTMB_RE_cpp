#include "lme2510/frontend/receiver.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include "firmware_embedded.h"
#include "lme2510/util/logger.hpp"
#include "lme2510/util/platform.hpp"

namespace lme2510 {
namespace {

std::vector<uint8_t> readFileBytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("firmware file not found: " + path);
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

std::vector<uint16_t> parsePidList(const std::string& text) {
  std::vector<uint16_t> pids;
  std::stringstream stream(text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (token.empty()) {
      continue;
    }
    // Trim whitespace.
    const std::size_t first = token.find_first_not_of(" \t\r\n");
    const std::size_t last = token.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue;
    }
    token = token.substr(first, last - first + 1);

    std::size_t pos = 0;
    unsigned long pid = 0;
    try {
      pid = std::stoul(token, &pos, 0);
    } catch (const std::exception&) {
      throw std::runtime_error("invalid PID in list: '" + token + "'");
    }
    if (pos != token.size() || pid > 0x1FFF) {
      throw std::runtime_error("invalid PID in list: '" + token + "'");
    }
    pids.push_back(static_cast<uint16_t>(pid));
  }
  if (pids.size() > 16) {
    throw std::runtime_error("PID count must be 1..16");
  }
  return pids;
}

void emitStatusLine(std::ostream* statusLog, const std::string& line) {
  std::cout << line << '\n';
  if (statusLog != nullptr) {
    *statusLog << nowTimestamp() << " | " << line << '\n';
    statusLog->flush();
  }
}

}  // namespace

Receiver::Receiver(const ReceiverOptions& options, RegLogger* regLogger,
                   std::ostream* statusLog)
    : options_(options),
      regLogger_(regLogger),
      statusLog_(statusLog),
      rawI2c_(bridge_, regLogger),
      gatedI2c_(rawI2c_),
      tuner_(gatedI2c_) {}

Receiver::~Receiver() {
  bridge_.close();
}

void Receiver::openLogged() {
  bridge_.setRegLogger(regLogger_);
  for (int attempt = 0; attempt < 20; ++attempt) {
    try {
      bridge_.open();
      return;
    } catch (const UsbError&) {
      if (attempt + 1 < 20) {
        sleepMilliseconds(250);
      }
    }
  }
  throw UsbError("LME2510C device not found");
}

void Receiver::reopenAfterFirmwareDownload() {
  for (int attempt = 0; attempt < 20; ++attempt) {
    try {
      bridge_.open();
      return;
    } catch (const UsbError&) {
      if (attempt + 1 < 20) {
        sleepMilliseconds(500);
      }
    }
  }
  throw UsbError("Device did not re-enumerate after firmware load");
}

bool Receiver::runConfiguration() {
  bridge_.setRegLogger(regLogger_);

  // 1. Open + firmware check/download.
  openLogged();
  if (bridge_.firmwareLoaded()) {
    std::cout << "Firmware: already loaded.\n";
  } else {
    std::cout << "Firmware: downloading stage-1 + stage-2...\n";

    Firmware fw1;
    Firmware fw2;
    std::vector<uint8_t> external1;
    std::vector<uint8_t> external2;
    if (options_.fw1Path.empty()) {
      fw1 = Firmware{embedded::kFwBootloader.data(), embedded::kFwBootloader.size(),
                     "fw_bootloader.bin (embedded)"};
    } else {
      external1 = readFileBytes(options_.fw1Path);
      fw1 = Firmware{external1.data(), external1.size(), options_.fw1Path};
    }
    if (options_.fw2Path.empty()) {
      fw2 = Firmware{embedded::kFwLgs8g75.data(), embedded::kFwLgs8g75.size(),
                     "fw_lgs8g75.bin (embedded)"};
    } else {
      external2 = readFileBytes(options_.fw2Path);
      fw2 = Firmware{external2.data(), external2.size(), options_.fw2Path};
    }

    bridge_.downloadFirmware(fw1, fw2);
    std::cout << "Waiting for device re-enumeration...\n";
    sleepSeconds(2.0);
    bridge_.close();
    reopenAfterFirmwareDownload();
    if (!bridge_.firmwareLoaded()) {
      std::cout << "Warning: firmware marker still missing; continuing anyway.\n";
    }
  }

  // 2. Identification + orchestrated initialization/tuning.
  initializeAndTune();

  // 3. PID-filter commit enables EP 0x8A/0x88 traffic.
  commitPidFilterAndSampleStatus();

  std::cout << "\nLocked: " << (locked_ ? "true" : "false")
            << " - forwarding TS on " << options_.frequencyMhz
            << " MHz...\n";
  return locked_;
}

void Receiver::initializeAndTune() {
  std::cout << "\n[Demodulator identification]\n";
  chip_ = Demodulator::identify(rawI2c_, 5, 0.5);

  if (chip_ == DemodChip::kLgs8Gl5) {
    demod_ = std::make_unique<Lgs8Gl5>(rawI2c_, chip_);
    bridge_.selectChipType(0);
    demod_->softReset();
  } else {
    demod_ = std::make_unique<Lgs8G75>(rawI2c_, chip_);
    bridge_.selectChipType(1);
  }

  std::cout << "\n[Tuner initialization]\n";
  tuner_.init();

  demod_->initAfterIdentify();
  // Python's tune() begins with a demodulator soft reset before the MAX2165
  // register writes; keep the ordering identical by doing that reset here.
  demod_->softReset();
  tuner_.tune(options_.frequencyMhz);

  locked_ = demod_->lockAfterTune();
}

void Receiver::commitPidFilterAndSampleStatus() {
  const std::vector<uint16_t> pids = parsePidList(options_.pidList);
  if (!pids.empty()) {
    std::cout << "\nPID filter: ";
    for (std::size_t i = 0; i < pids.size(); ++i) {
      if (i != 0) {
        std::cout << ", ";
      }
      std::cout << "0x" << hexWord(pids[i]);
    }
    std::cout << '\n';
    bridge_.sendPidFilter(pids, options_.pidMode);
  } else {
    bridge_.sendPidFilterDefaultAllPass();
  }

  const std::vector<uint8_t> raw = bridge_.readStatusTransfer(700);
  const std::optional<Ep8aStatus> status = parseFirstStatusPacket(raw);
  if (status) {
    emitStatusLine(statusLog_, "STATUS " + ep8aStatusLine(*status));
  } else {
    emitStatusLine(statusLog_,
                   "STATUS EP 0x8A: no packet within 700 ms");
  }
}

}  // namespace lme2510
