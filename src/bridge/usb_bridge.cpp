#include "lme2510/bridge/usb_bridge.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#pragma clang diagnostic ignored "-Wc99-extensions"
#endif
#include <libusb.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>

#include "lme2510/util/logger.hpp"
#include "lme2510/util/platform.hpp"

namespace lme2510 {
namespace {

const char* usbErrorName(int code) {
  return libusb_error_name(code);
}

}  // namespace

UsbBridge::UsbBridge() {
  const int rc = libusb_init(&context_);
  if (rc != LIBUSB_SUCCESS) {
    throw UsbError(std::string("libusb_init failed: ") +
                   usbErrorName(rc));
  }
}

UsbBridge::~UsbBridge() {
  close();
  if (context_ != nullptr) {
    libusb_exit(context_);
    context_ = nullptr;
  }
}

void UsbBridge::setRegLogger(RegLogger* logger) {
  regLogger_ = logger;
}

bool UsbBridge::isOpen() const {
  return handle_ != nullptr;
}

void UsbBridge::open() {
  if (handle_ != nullptr) {
    return;
  }
  findAndOpenDevice(kUsbPidWarm);
  if (handle_ == nullptr) {
    findAndOpenDevice(kUsbPidCold);
  }
  if (handle_ == nullptr) {
    throw UsbError(
        "Device not found (VID=0x3344, tried PID=0x1120 and 0x1111).\n"
        "  Windows: run Zadig and switch to WinUSB/libusb-win32.\n"
        "  Linux/macOS: ensure you have permission to access USB devices.");
  }

  libusb_device* device = libusb_get_device(handle_);

  // Detach a kernel driver if the platform exposes one.
  if (libusb_kernel_driver_active(handle_, 0) == 1) {
    libusb_detach_kernel_driver(handle_, 0);
  }

  int rc = libusb_set_configuration(handle_, 1);
  if (rc != LIBUSB_SUCCESS) {
    close();
    throw UsbError(std::string("libusb_set_configuration failed: ") +
                   usbErrorName(rc));
  }

  rc = libusb_claim_interface(handle_, 0);
  if (rc != LIBUSB_SUCCESS) {
    close();
    throw UsbError(std::string("libusb_claim_interface failed: ") +
                   usbErrorName(rc));
  }

  rc = libusb_set_interface_alt_setting(handle_, 0, 1);
  if (rc != LIBUSB_SUCCESS) {
    close();
    throw UsbError(std::string("set_interface_altsetting(0, 1) failed: ") +
                   usbErrorName(rc));
  }

  const int speed = libusb_get_device_speed(device);
  if (speed != LIBUSB_SPEED_HIGH) {
    close();
    std::ostringstream message;
    message << "Device is not High Speed (speed code " << speed
            << "). This build supports only EP 0x88, which exists in High "
               "Speed mode; Full Speed devices use EP 0x87 and are not "
               "supported by this v1 port.";
    throw UsbError(message.str());
  }

  uint16_t pid = 0;
  libusb_device_descriptor descriptor{};
  if (libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS) {
    pid = descriptor.idProduct;
  }
  std::cout << "Device opened: 0x3344:0x" << std::hex << std::uppercase
            << pid << std::dec << "  (Alt Setting 1)\n";
}

void UsbBridge::close() {
  if (handle_ != nullptr) {
    libusb_close(handle_);
    handle_ = nullptr;
  }
}

void UsbBridge::findAndOpenDevice(uint16_t pid) {
  if (handle_ != nullptr || context_ == nullptr) {
    return;
  }
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context_, &devices);
  if (count < 0) {
    throw UsbError(std::string("libusb_get_device_list failed: ") +
                   usbErrorName(static_cast<int>(count)));
  }

  for (ssize_t i = 0; i < count; ++i) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[i], &descriptor) !=
        LIBUSB_SUCCESS) {
      continue;
    }
    if (descriptor.idVendor != kUsbVid || descriptor.idProduct != pid) {
      continue;
    }

    const int rc = libusb_open(devices[i], &handle_);
    if (rc == LIBUSB_SUCCESS) {
      break;
    }
    throw UsbError(std::string("libusb_open failed (") +
                   usbErrorName(rc) + ")");
  }

  libusb_free_device_list(devices, 1);
}

bool UsbBridge::writeRaw(const std::vector<uint8_t>& data) {
  if (regLogger_ != nullptr && regLogger_->usbTrace()) {
    regLogger_->log("USB_TX", hexBytes(data));
  }

  int transferred = 0;
  const int rc = libusb_bulk_transfer(
      handle_, kEpCmdOut, const_cast<uint8_t*>(data.data()),
      static_cast<int>(data.size()), &transferred, 1000);
  if (rc != LIBUSB_SUCCESS || transferred != static_cast<int>(data.size())) {
    throw UsbError(std::string("USB TX failed: ") + usbErrorName(rc));
  }
  return true;
}

std::vector<uint8_t> UsbBridge::readRaw(std::size_t length, int timeoutMs) {
  std::vector<uint8_t> buffer(length, 0);
  int transferred = 0;
  const int rc =
      libusb_bulk_transfer(handle_, kEpCmdIn, buffer.data(),
                           static_cast<int>(length), &transferred, timeoutMs);
  if (rc == LIBUSB_ERROR_TIMEOUT) {
    buffer.clear();
  } else if (rc != LIBUSB_SUCCESS) {
    buffer.clear();
  } else {
    buffer.resize(static_cast<std::size_t>(transferred));
  }
  if (regLogger_ != nullptr && regLogger_->usbTrace()) {
    regLogger_->log("USB_RX", buffer.empty()
                                  ? "(none)"
                                  : hexBytes(buffer));
  }
  return buffer;
}

bool UsbBridge::writeBlock(uint8_t devAddr, uint8_t regAddr,
                           const std::vector<uint8_t>& data) {
  if (data.size() > 255 - 2) {
    throw UsbError("writeBlock payload too large");
  }
  std::vector<uint8_t> packet;
  packet.reserve(4 + data.size());
  packet.push_back(0x04);
  packet.push_back(static_cast<uint8_t>(2 + data.size()));
  packet.push_back(devAddr);
  packet.push_back(regAddr);
  packet.insert(packet.end(), data.begin(), data.end());

  std::lock_guard<std::mutex> lock(controlMutex_);
  writeRaw(packet);
  const std::vector<uint8_t> ack = readRaw(4, 1000);
  return !ack.empty() && ack[0] == 0x88;
}

bool UsbBridge::writeSingle(uint8_t devAddr, uint8_t regAddr, uint8_t value) {
  const std::vector<uint8_t> packet{0x05, 0x04, devAddr, regAddr, value};
  std::lock_guard<std::mutex> lock(controlMutex_);
  writeRaw(packet);
  const std::vector<uint8_t> ack = readRaw(4, 1000);
  return !ack.empty() && ack[0] == 0x88;
}

std::optional<uint8_t> UsbBridge::readSingle(uint8_t devAddr, uint8_t regAddr) {
  const std::vector<uint8_t> packet{0x85, 0x02, devAddr, regAddr, 0x00};
  std::lock_guard<std::mutex> lock(controlMutex_);
  writeRaw(packet);
  const std::vector<uint8_t> response = readRaw(5, 1000);
  if (response.size() >= 2 && response[0] == 0x55) {
    return response[1];
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> UsbBridge::readBlock(
    uint8_t devAddr, uint8_t regAddr, std::size_t count) {
  if (count > 255) {
    throw UsbError("readBlock count too large");
  }
  const std::vector<uint8_t> packet{0x84, 0x03, devAddr, regAddr,
                                    static_cast<uint8_t>(count)};
  std::lock_guard<std::mutex> lock(controlMutex_);
  writeRaw(packet);
  const std::vector<uint8_t> response = readRaw(count + 1, 1000);
  if (response.size() >= 1 && response[0] == 0x55) {
    std::vector<uint8_t> data(response.begin() + 1, response.end());
    return data;
  }
  return std::nullopt;
}

std::vector<uint8_t> UsbBridge::sendCommand(
    const std::vector<uint8_t>& packet, std::size_t ackLength,
    const std::string& label) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  writeRaw(packet);
  const std::vector<uint8_t> response = readRaw(ackLength, 1000);

  std::cout << "  " << label << ": " << hexBytes(packet) << '\n';
  std::cout << "    ACK: "
            << (response.empty() ? "(none)" : hexBytes(response)) << '\n';
  if (regLogger_ != nullptr && regLogger_->enabled()) {
    regLogger_->log("CTRL_CMD",
                    label + " packet=" + hexBytes(packet) +
                        " ack=" +
                        (response.empty() ? "(none)" : hexBytes(response)));
  }
  return response;
}

bool UsbBridge::selectChipType(uint8_t chipType) {
  const std::vector<uint8_t> response =
      sendCommand({0x16, 0x01, chipType}, 5, "CMD16 chip type");
  return !response.empty();
}

bool UsbBridge::sendPidFilter(const std::vector<uint16_t>& pids, int mode) {
  if (pids.empty() || pids.size() > 16) {
    throw UsbError("PID count must be 1..16");
  }
  for (uint16_t pid : pids) {
    if (pid > 0x1FFF) {
      throw UsbError("PID values must be 0x0000..0x1FFF");
    }
  }

  std::vector<uint8_t> packet;
  packet.push_back(0x03);
  packet.push_back(static_cast<uint8_t>(4 * pids.size() + 2));
  for (std::size_t k = 0; k < pids.size(); ++k) {
    packet.push_back(static_cast<uint8_t>(2 * k));
    packet.push_back(static_cast<uint8_t>(pids[k] & 0xFF));
    packet.push_back(static_cast<uint8_t>(2 * k + 1));
    packet.push_back(static_cast<uint8_t>(pids[k] >> 8));
  }
  packet.push_back(0x20);
  const uint8_t terminal = static_cast<uint8_t>(
      (mode == 2 ? 0x81 : 0x80) + 2 * (pids.size() - 1));
  packet.push_back(terminal);

  const std::vector<uint8_t> ack1 =
      sendCommand(packet, 5, "CMD03 PID filter pass 1");
  const std::vector<uint8_t> ack2 =
      sendCommand(packet, 5, "CMD03 PID filter pass 2");
  const std::vector<uint8_t> ack3 =
      sendCommand({0x06, 0x00}, 5, "CMD06 commit");
  return !ack1.empty() && !ack2.empty() && !ack3.empty();
}

bool UsbBridge::sendPidFilterDefaultAllPass() {
  return sendPidFilter({0x1FFF}, 2);
}

std::vector<uint8_t> UsbBridge::readStreamChunk(std::size_t bufSize,
                                                int timeoutMs) {
  if (handle_ == nullptr) {
    throw UsbError("USB device is not open");
  }
  std::vector<uint8_t> buffer(bufSize, 0);
  int transferred = 0;
  const int rc = libusb_bulk_transfer(
      handle_, kEpStream, buffer.data(), static_cast<int>(buffer.size()),
      &transferred, timeoutMs);
  if (rc == LIBUSB_ERROR_TIMEOUT) {
    return {};
  }
  if (rc != LIBUSB_SUCCESS) {
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      throw UsbError("USB device disconnected");
    }
    throw UsbError(std::string("EP 0x88 read failed: ") + usbErrorName(rc));
  }
  buffer.resize(static_cast<std::size_t>(transferred));
  return buffer;
}

std::vector<uint8_t> UsbBridge::readStatusTransfer(int timeoutMs) {
  if (handle_ == nullptr) {
    throw UsbError("USB device is not open");
  }
  std::vector<uint8_t> buffer(64, 0);
  int transferred = 0;
  const int rc = libusb_interrupt_transfer(
      handle_, kEpStatus, buffer.data(), static_cast<int>(buffer.size()),
      &transferred, timeoutMs);
  if (rc == LIBUSB_ERROR_TIMEOUT) {
    return {};
  }
  if (rc != LIBUSB_SUCCESS) {
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      throw UsbError("USB device disconnected");
    }
    return {};
  }
  buffer.resize(static_cast<std::size_t>(transferred));
  return buffer;
}

bool UsbBridge::firmwareLoaded() {
  if (handle_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(controlMutex_);
  std::vector<uint8_t> buffer(255, 0);
  const int rc = libusb_control_transfer(
      handle_, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, (3 << 8) | 2, 0x0409,
      buffer.data(), static_cast<uint16_t>(buffer.size()), 1000);
  if (rc < 0) {
    return false;
  }
  int gCount = 0;
  for (int i = 2; i < rc; ++i) {
    if (buffer[static_cast<std::size_t>(i)] == 0x47) {
      ++gCount;
    }
  }
  return gCount >= 3;
}

void UsbBridge::downloadFirmware(const Firmware& fw1, const Firmware& fw2) {
  std::lock_guard<std::mutex> lock(controlMutex_);
  downloadStage(fw1, 1);
  sleepMilliseconds(100);
  downloadStage(fw2, 2);
  sleepMilliseconds(500);
  postFirmwareCommand();
}

void UsbBridge::downloadStage(const Firmware& fw, uint8_t fwId) {
  if (fw.empty()) {
    throw UsbError("empty firmware blob");
  }
  std::cout << "  [" << static_cast<int>(fwId) << "] " << fw.source << "  ("
            << fw.size << " bytes)\n";

  const uint8_t baseCommand = fwId & 0x7F;
  std::size_t offset = 0;
  while (offset < fw.size) {
    const std::size_t remaining = fw.size - offset;
    const std::size_t chunkSize = std::min<std::size_t>(50, remaining);
    const bool isLast = offset + chunkSize >= fw.size;
    const uint8_t cmd = static_cast<uint8_t>(baseCommand | (isLast ? 0x80 : 0));

    std::vector<uint8_t> packet;
    packet.reserve(2 + chunkSize + 1);
    packet.push_back(cmd);
    packet.push_back(static_cast<uint8_t>(chunkSize - 1));
    uint8_t checksum = 0;
    for (std::size_t i = 0; i < chunkSize; ++i) {
      const uint8_t byte = fw.data[offset + i];
      packet.push_back(byte);
      checksum = static_cast<uint8_t>(checksum + byte);
    }
    packet.push_back(checksum);

    writeRaw(packet);
    const std::vector<uint8_t> ack = readRaw(1, 1000);
    if (ack.empty() ||
        (ack[0] != kFwAck1 && ack[0] != kFwAck2)) {
      throw UsbError("FW" + std::to_string(static_cast<int>(fwId)) +
                     " upload failed at offset " + std::to_string(offset));
    }
    offset += chunkSize;
  }
  std::cout << "     -> done\n";
}

void UsbBridge::postFirmwareCommand() {
  try {
    writeRaw({0x8A, 0x00});
    readRaw(5, 1000);
  } catch (const UsbError&) {
    // Device reset/re-enumeration can abort the response read; that is normal.
  }
}

void UsbBridge::printUsbDescriptors() {
  // Kept intentionally small: open() already validates the endpoints we need.
  libusb_device* device = libusb_get_device(handle_);
  libusb_device_descriptor descriptor{};
  if (libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS) {
    std::cout << "[USB descriptors]\n";
    std::cout << "  VID:PID = 0x" << std::hex << std::uppercase
              << descriptor.idVendor << ":0x" << descriptor.idProduct
              << std::dec << '\n';
  }
}

}  // namespace lme2510
