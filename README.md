# CH1_DTMB_RE C++17 Port

A cross-platform C++17 reimplementation of the CH1 DTMB receiver workflow
previously prototyped in `lme2510_stream.py`.  It drives the same
LME2510C USB bridge (VID/PID `0x3344` / `0x1120`) with an LGS8GL5 demodulator
and a MAX2165 tuner, and forwards the demodulated MPEG-TS stream over UDP,
raw UDP, and/or a `.ts` file.

This repository contains the firmware blobs extracted from the original
Windows driver and embeds them into the executable at build time.  No Python
runtime is needed.

## Supported hardware

- CH1 DTMB stick with LME2510C bridge firmware.
- Demodulator: Legend Silicon LGS8GL5.
- Tuner: Maxim MAX2165.
- TS output endpoint: EP `0x88` (High Speed only).  A Full Speed device would
  use a different stream endpoint and is intentionally rejected/unsupported in
  this version.

Protocol reference: [LME2510_Analysis.md](LME2510_Analysis.md).  Upstream
kernel driver: <https://github.com/torvalds/linux/blob/master/drivers/media/usb/dvb-usb-v2/lmedm04.c>.

## Build

### macOS

```sh
brew install libusb pkg-config cmake
cmake -S . -B build
cmake --build build
```

USB access on macOS normally requires root:

```sh
sudo ./build/lme2510_stream --freq 618 --no-udp --file out.ts --seconds 10
```

### Linux

```sh
sudo apt install cmake build-essential pkg-config libusb-1.0-0-dev
cmake -S . -B build
cmake --build build
```

If the device is not readable by your user, install a udev rule or run with
`sudo`.  A minimal rule is:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="3344", ATTR{idProduct}=="1120", MODE="0666"
```

### Windows

Install [vcpkg](https://vcpkg.io), libusb, and CMake, then configure with the
vcpkg toolchain:

```powershell
vcpkg install libusb:x64-windows
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

Use [Zadig](https://zadig.akeo.ie) to replace the stock Windows driver for the
device (VID/PID `0x3344` / `0x1120`) with WinUSB or libusbK.  Run:

```powershell
.\build\Release\lme2510_stream.exe --freq 618 --no-udp --file out.ts --seconds 10
```

## Usage

```text
lme2510_stream [options]

  --freq MHz            tune frequency in MHz (default 618)
  --pids LIST           comma-separated PID allow-list, e.g. 0x0100,0x0101
  --pid-mode N          CMD 0x03 mode (0 = allow-list, 2 = clear/advanced)
  --udp HOST:PORT       UDP target (default 127.0.0.1:1234)
  --raw-udp HOST:PORT   send raw EP 0x88 bulk frames unchanged
  --no-udp              disable UDP
  --file PATH           write packetized TS to PATH
  --seconds N           stop after N seconds (default: until Ctrl-C)
  --telemetry N         register snapshot interval in seconds (0 disables)
  --live-status         read EP 0x8A on a background thread while streaming
  --status-log PATH     status/statistics log (default logs/stream-<time>.log)
  --reg-log PATH        register-operation log (default logs/regs-<time>.log)
  --usb-trace           include raw USB packets in the register log
  --fw1 PATH            override embedded stage-1 firmware
  --fw2 PATH            override embedded stage-2 firmware
```

Examples:

```sh
# TS to UDP 127.0.0.1:1234 (default)
sudo ./build/lme2510_stream --freq 618

# Raw EP 0x88 frames to a separate diagnostic port
sudo ./build/lme2510_stream --freq 618 --raw-udp 127.0.0.1:1235

# Ten seconds of PID-filtered 554 MHz into a file, no UDP
sudo ./build/lme2510_stream --freq 554 --pids 0x0100,0x0101 --no-udp \
  --file ts554.ts --seconds 10 --reg-log logs/regs-554.log
```

Play the UDP stream in VLC with `udp://@1234`.  File captures are standard
188-byte MPEG-TS and can be played by any TS-aware player.

## Notes

- Embedded stage-2 firmware keeps the original `fw_lgs8g75.bin` bridge blob
  used by the Python workflow; the filename predates this project and is not a
  claim of LGS8G75 hardware support.  The demodulator validated by this port
  is LGS8GL5.  Pass `--fw2` to load a different blob.
- Status/register logs are created under `logs/` on every run.
- The main thread continuously drains EP `0x88`; register telemetry and status
  reads happen only when the stream loop is idle or after it stops, unless
  `--live-status` is used.

## Compatibility notes

Only the combination validated by this project is claimed as supported:

- LME2510C with USB VID/PID `0x3344` / `0x1120`
- LGS8GL5 demodulator
- MAX2165 tuner
- High Speed USB with EP `0x88` as the TS endpoint

The LGS8G75 branch in the protocol/initialization code has not been validated
against hardware in this port and is therefore not claimed as supported.
Firmware download also searches for cold-boot PID `0x1111` before the stage-1
firmware is loaded; that PID is only a transient boot state, not a supported
device identity.
