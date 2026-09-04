# 第一波道 DTMB USB 电视棒用户态客户端（CH1_DTMB_RE C++17 Port）

<div align="center">
  <img src="assets/cmdline.png" alt="cmdline" width="46%" />
  <img src="assets/ffplay.png" alt="ffplay" width="46%" />
</div>



Cross-platform C++17 program that drives a CH1 DTMB receiver (LME2510C USB
bridge, VID/PID `0x3344` / `0x1120`, LGS8GL5 demodulator, MAX2165 tuner) and
forwards the demodulated MPEG-TS stream over UDP, raw UDP, and/or a `.ts` file.
Firmware blobs extracted from the original Windows driver are embedded into the
executable at build time.

```mermaid
flowchart LR
  subgraph HW["Hardware - CH1 DTMB USB stick"]
    RF["DTMB RF signal<br/>(antenna / cable)"] --> TUNER["MAX2165 tuner"]
    TUNER --> DEMOD["LGS8GL5 demodulator<br/>MPEG-TS output"]
    DEMOD --> BRIDGE["LME2510C USB bridge<br/>VID 0x3344 / PID 0x1120"]
  end

  subgraph USB["USB protocol layer (High Speed, via libusb)"]
    CMDEP["Control pipe<br/>EP 0x01 OUT / EP 0x81 IN (Bulk)<br/>firmware download, I2C read/write, ACK"]
    TSEP["TS pipe<br/>EP 0x88 IN (Bulk)<br/>continuous MPEG-TS byte stream"]
    STSEP["Status pipe<br/>EP 0x8A IN (Interrupt)<br/>signal / lock status"]
  end

  subgraph APP["lme2510_stream (this program)"]
    CTRL["Control plane: Receiver<br/>open device, init / tune, PID filter"]
    LOOP["Data plane: StreamSession<br/>read EP 0x88 and align<br/>188-byte TS packets (TsPacketizer)"]
    UDPS["UDP sink<br/>1316-byte datagrams<br/>(default 127.0.0.1:1234)"]
    FILES["File sink<br/>write packetized .ts file"]
    STAT["Status reader<br/>(optional --live-status)"]
  end

  subgraph DOWN["Downstream"]
    VLC["VLC / ffplay / other TS apps<br/>listen on udp://@1234"]
    FPLAY["VLC / TS-aware players<br/>open the saved .ts"]
  end

  CTRL <--> CMDEP <--> BRIDGE
  BRIDGE --> TSEP --> LOOP
  BRIDGE --> STSEP --> STAT
  LOOP --> UDPS
  LOOP --> FILES
  UDPS --> VLC
  FILES --> FPLAY
```

## Supported hardware

- CH1 DTMB stick with LME2510C bridge firmware.
- Demodulator: Legend Silicon LGS8GL5.
- Tuner: Maxim MAX2165.
- TS output endpoint: EP `0x88`.

Protocol reference: [LME2510_Analysis.md](LME2510_Analysis.md).

## Building from source

Dependency install, CMake configuration, and cross-compilation instructions
live in [BUILD.md](BUILD.md).

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
  --tui                 full-screen ncurses UI (macOS/Linux)
  --fw1 PATH            override embedded stage-1 firmware
  --fw2 PATH            override embedded stage-2 firmware
```

Examples:

```sh
# TS to UDP 127.0.0.1:1234 (default)
sudo ./build/lme2510_stream --freq 618

# Interactive TUI: pick a frequency, watch signal, then stream/record
sudo ./build/lme2510_stream --tui

# Raw EP 0x88 frames to a separate diagnostic port
sudo ./build/lme2510_stream --freq 618 --raw-udp 127.0.0.1:1235

# Ten seconds of PID-filtered 554 MHz into a file, no UDP
sudo ./build/lme2510_stream --freq 554 --pids 0x0100,0x0101 --no-udp \
  --file ts554.ts --seconds 10 --reg-log logs/regs-554.log
```

Play the UDP stream in VLC with `udp://@1234`.  File captures are standard
188-byte MPEG-TS and can be played by any TS-aware player.

### TUI (`--tui`)

The optional full-screen UI lets you:

- Walk the Chinese DTMB UHF grid (474–858 MHz in 8 MHz steps), mark
  frequencies with `Space`, scan the marked set with `S`, and tune a row with
  `Enter`.
- Watch live EP 0x8A signal strength/quality, register telemetry, TS
  continuity-counter losses, resync/dropped-byte counters, and bit rates.
- Switch into the service list (`C` from Monitor) once PAT/PMT tables are
  parsed, choose a TV/radio service, and have UDP streaming and file recording
  PID-filter that service.  `C` in the service list clears back to the whole
  multiplex.
- Browse the EPG (`E` in the service list, or `4` once monitoring): service
  names come from SDT and the now/next plus scheduled programme list from the
  EIT.  Chinese DTMB names are GB2312 and are converted to UTF-8 with a built-in
  table, so no iconv dependency is needed.
- Toggle UDP streaming with `Space` and file recording with `R`.  If no
  `--file` target is supplied, recording creates `record-<unix-time>.ts` in
  the current directory.
- Switch to a live command log (`Tab`) showing the CTRL/I2C commands sent to
  the stick; it tails the `--reg-log` file.

`--tui` is only built on macOS/Linux (ncurses).  Windows builds keep the CLI
behaviour and print an error if `--tui` is requested.

### Platform access

#### macOS

USB access on macOS normally requires root:

```sh
sudo ./build/lme2510_stream --freq 618 --no-udp --file out.ts --seconds 10
```

#### Linux

If the device is not readable by your user, install a udev rule or run with
`sudo`.  A minimal rule is:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="3344", ATTR{idProduct}=="1120", MODE="0666"
```

#### Windows

Use [Zadig](https://zadig.akeo.ie) to replace the stock Windows driver for the
device (VID/PID `0x3344` / `0x1120`) with WinUSB or libusbK.  Run:

```powershell
.\build\Release\lme2510_stream.exe --freq 618 --no-udp --file out.ts --seconds 10
```

#### Windows XP

- Windows XP is normally 32-bit, so use the x86 artifact
  (`lme2510_stream-xp-x86.exe`).
- Microsoft's WinUSB is only available from Vista onwards.  On XP, replace the
  stock driver with a **libusb-win32** or **libusbK** driver; old
  [Zadig 2.2](https://github.com/pbatard/libwdi/releases/download/v1.2.5/zadig_xp-2.2.exe) is the
  version that still supports XP.

## Compatibility notes

- Full Speed USB devices use a different TS stream endpoint than EP `0x88` and
  are intentionally rejected/unsupported in this version.
- The LGS8G75 branch in the protocol/initialization code has not been validated
  against hardware in this port and is not claimed as supported.
- Firmware download searches for cold-boot PID `0x1111` before the stage-1
  firmware is loaded; that PID is only a transient boot state, not a supported
  device identity.

## Acknowledgements

- [Linux Kernel Driver for LME2510C](https://github.com/torvalds/linux/blob/master/drivers/media/usb/dvb-usb-v2/lmedm04.c)
- [LeDTMB](https://github.com/IcingTomato/LeDTMB): Client for a rather simple DTMB receiver.
- [libusb](https://libusb.info/): Library for USB device access in userspace.
