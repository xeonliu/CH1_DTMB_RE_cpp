#include "lme2510/util/arg_parser.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace lme2510 {

std::string helpText() {
  return R"(lme2510_stream — tune the CH1 (LME2510C + LGS8GL5 + MAX2165) DTMB stick,
enable the transport stream, and forward it to UDP and/or a .ts file.

Usage: lme2510_stream [options]

Options:
  -h, --help           show this help message and exit
  --freq MHz           tune frequency in MHz (default: 618)
  --pids LIST          comma-separated PID list (hex, e.g. 0x0100,0x0101) to
                       program into the bridge filter as an allow-list;
                       default 0x1FFF = all PIDs
  --pid-mode N         CMD 0x03 mode: 0 = keep only listed PIDs (default);
                       2 = clear/0x1FFF semantics (advanced)
  --udp HOST:PORT      UDP target (default: 127.0.0.1:1234)
  --raw-udp HOST:PORT  forward each raw EP 0x88 bulk frame unchanged
  --no-udp             disable UDP
  --file PATH          write TS to this file
  --seconds N          stop after N seconds (default: until Ctrl-C)
  --telemetry N        live register snapshot interval (default: 2.0; 0 disables)
  --live-status        read EP 0x8A on a background thread while streaming
  --status-log PATH    status log (default: logs/stream-<time>.log)
  --reg-log PATH       register log (default: logs/regs-<time>.log)
  --usb-trace          also log raw USB TX/RX packets
  --fw1 PATH           override embedded stage-1 firmware
  --fw2 PATH           override embedded stage-2 firmware
)";
}

void printHelp() {
  std::cout << helpText();
}

namespace {

bool nextValue(int argc, char** argv, int& i, const std::string& option,
               std::string& out, std::string& error) {
  if (i + 1 >= argc) {
    error = "argument --" + option + ": expected one argument";
    return false;
  }
  out = argv[++i];
  return true;
}

bool parseInt(const std::string& text, const std::string& option, int& out,
              std::string& error) {
  try {
    std::size_t pos = 0;
    long v = std::stol(text, &pos, 0);
    if (pos != text.size()) {
      throw std::invalid_argument("trailing");
    }
    if (v < static_cast<long>(INT32_MIN) ||
        v > static_cast<long>(INT32_MAX)) {
      throw std::out_of_range("range");
    }
    out = static_cast<int>(v);
    return true;
  } catch (const std::exception&) {
    error = "argument --" + option + ": invalid int value: '" + text + "'";
    return false;
  }
}

bool parseDouble(const std::string& text, const std::string& option, double& out,
                 std::string& error) {
  try {
    std::size_t pos = 0;
    out = std::stod(text, &pos);
    if (pos != text.size() || !std::isfinite(out)) {
      throw std::invalid_argument("trailing");
    }
    return true;
  } catch (const std::exception&) {
    error = "argument --" + option + ": invalid float value: '" + text + "'";
    return false;
  }
}

}  // namespace

bool parseArgs(int argc, char** argv, Options& out, std::string& error,
               bool& showHelp) {
  showHelp = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      showHelp = true;
      return true;
    }
    if (arg.rfind("--", 0) != 0) {
      error = "unrecognized argument: '" + arg + "'";
      return false;
    }

    std::string name = arg.substr(2);
    std::string inlineValue;
    bool hasInline = false;
    const std::size_t eq = name.find('=');
    if (eq != std::string::npos) {
      inlineValue = name.substr(eq + 1);
      name = name.substr(0, eq);
      hasInline = true;
    }

    auto take = [&](const std::string& opt, std::string& value) -> bool {
      if (hasInline) {
        value = inlineValue;
        return true;
      }
      return nextValue(argc, argv, i, opt, value, error);
    };

    std::string value;
    if (name == "freq") {
      if (!take(name, value) || !parseInt(value, name, out.frequencyMhz, error)) {
        return false;
      }
    } else if (name == "pids") {
      if (!take(name, value)) return false;
      out.pids = value;
    } else if (name == "pid-mode") {
      if (!take(name, value) || !parseInt(value, name, out.pidMode, error)) {
        return false;
      }
    } else if (name == "udp") {
      if (!take(name, value)) return false;
      out.udpTarget = value;
    } else if (name == "raw-udp") {
      if (!take(name, value)) return false;
      out.rawUdpTarget = value;
    } else if (name == "no-udp") {
      if (hasInline) {
        error = "argument --no-udp: ignored explicit argument";
        return false;
      }
      out.noUdp = true;
    } else if (name == "file") {
      if (!take(name, value)) return false;
      out.filePath = value;
    } else if (name == "seconds") {
      if (!take(name, value) || !parseDouble(value, name, out.seconds, error)) {
        return false;
      }
    } else if (name == "telemetry") {
      if (!take(name, value) ||
          !parseDouble(value, name, out.telemetryInterval, error)) {
        return false;
      }
    } else if (name == "live-status") {
      if (hasInline) {
        error = "argument --live-status: ignored explicit argument";
        return false;
      }
      out.liveStatus = true;
    } else if (name == "status-log") {
      if (!take(name, value)) return false;
      out.statusLogPath = value;
    } else if (name == "reg-log") {
      if (!take(name, value)) return false;
      out.regLogPath = value;
    } else if (name == "usb-trace") {
      if (hasInline) {
        error = "argument --usb-trace: ignored explicit argument";
        return false;
      }
      out.usbTrace = true;
    } else if (name == "fw1") {
      if (!take(name, value)) return false;
      out.fw1Path = value;
    } else if (name == "fw2") {
      if (!take(name, value)) return false;
      out.fw2Path = value;
    } else {
      error = "unrecognized argument: '--" + name + "'";
      return false;
    }
  }
  return true;
}

}  // namespace lme2510
