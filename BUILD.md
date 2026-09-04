# Building lme2510_stream from source

## macOS

```sh
brew install libusb pkg-config cmake
cmake -S . -B build
cmake --build build
```

## Linux

```sh
sudo apt install cmake build-essential pkg-config libusb-1.0-0-dev
cmake -S . -B build
cmake --build build
```

## Windows

Install [vcpkg](https://vcpkg.io), libusb, and CMake, then configure with the
vcpkg toolchain:

```powershell
vcpkg install libusb:x64-windows
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

## Windows XP (32-bit)

GitHub Actions has no Windows XP runners, so the XP artifact is
cross-compiled on Linux with an i686 MinGW-w64 toolchain (see
`.github/workflows/ci.yml`).  The workflow pins libusb to **1.0.23**, the last
release that still supported Windows XP, and links the compiler runtime
statically so the resulting `.exe` has no extra DLL dependencies.

To build an XP artifact locally on Ubuntu 22.04:

```sh
sudo apt-get install g++-mingw-w64-i686-posix libusb-1.0-0-dev pkg-config

ROOT="$PWD"
XP_PREFIX="$ROOT/build/xp-libusb-prefix"

# 1. Build a host-native copy of the firmware generator.
cmake -S . -B build/host-gen -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-gen --target gen_firmware_embed

# 2. Build the last XP-capable libusb release for i686 Windows.
cd "$(mktemp -d)"
curl -fL -O https://github.com/libusb/libusb/releases/download/v1.0.23/libusb-1.0.23.tar.bz2
tar -xjf libusb-1.0.23.tar.bz2
cd libusb-1.0.23
./configure --host=i686-w64-mingw32 --prefix="$XP_PREFIX" \
  --enable-static --disable-shared --disable-udev \
  CFLAGS="-O2 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501"
make -C libusb -j"$(nproc)"
make -C libusb install
cd "$ROOT"

# 3. Configure and build the XP binary.
cmake -S . -B build/xp \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw-i686-xp-toolchain.cmake" \
  -DLME2510_HOST_GEN_FIRMWARE_EMBED="$PWD/build/host-gen/gen_firmware_embed" \
  -DLIBUSB_INCLUDE_DIR="$XP_PREFIX/include/libusb-1.0" \
  -DLIBUSB_LIBRARY="$XP_PREFIX/lib/libusb-1.0.a"
cmake --build build/xp
```
