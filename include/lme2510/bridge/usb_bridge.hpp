#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace lme2510 {

class RegLogger;

// USB identity and endpoint layout (see LME2510_Analysis.md).
inline constexpr uint16_t kUsbVid = 0x3344;
inline constexpr uint16_t kUsbPidCold = 0x1111;
inline constexpr uint16_t kUsbPidWarm = 0x1120;
inline constexpr uint8_t kEpCmdOut = 0x01;
inline constexpr uint8_t kEpCmdIn = 0x81;
inline constexpr uint8_t kEpStream = 0x88;
inline constexpr uint8_t kEpStatus = 0x8A;

// I2C logical addresses used across the demod/tuner layers.
inline constexpr uint8_t kDemodAddr = 0x32;
inline constexpr uint8_t kDemodHighAddr = 0x36;
inline constexpr uint8_t kTunerAddr = 0xC0;
inline constexpr int kRefFreqMhz = 12;

// Firmware ACKs accepted by the original driver.
inline constexpr uint8_t kFwAck1 = 0x88;
inline constexpr uint8_t kFwAck2 = 0x77;

struct Firmware {
  const uint8_t* data = nullptr;
  std::size_t size = 0;
  std::string source;

  bool empty() const { return data == nullptr || size == 0; }
};

/// Error thrown for USB/protocol failures that should abort the session.
class UsbError : public std::runtime_error {
 public:
  explicit UsbError(const std::string& message) : std::runtime_error(message) {}
};

/// Low-level LME2510C bridge wrapper.  Owns the libusb context/handle and the
/// control-plane mutex.  Does not interpret demodulator or tuner register
/// semantics.
class UsbBridge {
 public:
  UsbBridge();
  ~UsbBridge();

  UsbBridge(const UsbBridge&) = delete;
  UsbBridge& operator=(const UsbBridge&) = delete;

  /// Tries warm PID 0x1120 first, then cold PID 0x1111.  Detaches a kernel
  /// driver if needed, sets configuration 1, claims interface 0 and activates
  /// Alternate Setting 1.
  void open();
  void close();
  bool isOpen() const;

  void setRegLogger(RegLogger* logger);

  /// Reads string descriptor 2 raw and reports whether the 'G' warm marker is
  /// present (>= 3 bytes of 0x47).
  bool firmwareLoaded();

  /// Downloads two firmware stages and posts the activation command.
  void downloadFirmware(const Firmware& fw1, const Firmware& fw2);

  /// CMD 0x16. chipType: 0 = LGS8GL5, 1 = LGS8G75.
  bool selectChipType(uint8_t chipType);

  // I2C protocol commands ---------------------------------------------------
  bool writeBlock(uint8_t devAddr, uint8_t regAddr,
                  const std::vector<uint8_t>& data);
  bool writeSingle(uint8_t devAddr, uint8_t regAddr, uint8_t value);
  std::optional<uint8_t> readSingle(uint8_t devAddr, uint8_t regAddr);
  std::optional<std::vector<uint8_t>> readBlock(uint8_t devAddr,
                                                uint8_t regAddr,
                                                std::size_t count);

  // PID filter (CMD 0x03 twice + CMD 0x06 commit).
  bool sendPidFilter(const std::vector<uint16_t>& pids, int mode);
  bool sendPidFilterDefaultAllPass();

  /// Raw command sender used for logging-friendly control commands.
  std::vector<uint8_t> sendCommand(const std::vector<uint8_t>& packet,
                                   std::size_t ackLength,
                                   const std::string& label);

  /// Reads one Bulk IN transfer from EP 0x88.  Returns an empty vector on a
  /// timeout, matching read_stream_chunk()'s b"" semantics.
  std::vector<uint8_t> readStreamChunk(std::size_t bufSize = 4096,
                                       int timeoutMs = 500);

  /// Reads one interrupt transfer from EP 0x8A.  Returns an empty vector on a
  /// timeout.  Callers parse 0xBB packets from the buffer.
  std::vector<uint8_t> readStatusTransfer(int timeoutMs = 700);

  void printUsbDescriptors();

 private:
  bool writeRaw(const std::vector<uint8_t>& data);
  std::vector<uint8_t> readRaw(std::size_t length, int timeoutMs = 1000);
  void downloadStage(const Firmware& fw, uint8_t fwId);
  void postFirmwareCommand();
  void findAndOpenDevice(uint16_t pid);
  std::string lastErrorName(int err) const;

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  std::mutex controlMutex_;
  RegLogger* regLogger_ = nullptr;
};

}  // namespace lme2510
