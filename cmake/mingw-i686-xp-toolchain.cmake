# CMake toolchain for cross-compiling 32-bit Windows XP binaries with the
# MinGW-w64 "win32 threads" compiler on a Linux host.
#
# Install the toolchain first (Ubuntu):
#   sudo apt-get install g++-mingw-w64-i686-win32
#
# The firmware generator is a native host tool, so cross builds need a
# host-native copy built beforehand:
#   cmake -S . -B build/host-gen -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/host-gen --target gen_firmware_embed
#
# Example configure:
#   cmake -S . -B build/xp \
#     -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mingw-i686-xp-toolchain.cmake \
#     -DLME2510_HOST_GEN_FIRMWARE_EMBED=$PWD/build/host-gen/gen_firmware_embed \
#     -DLIBUSB_INCLUDE_DIR=$PWD/xp-prefix/include/libusb-1.0 \
#     -DLIBUSB_LIBRARY=$PWD/xp-prefix/lib/libusb-1.0.a

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)

# Only declare/use APIs available on Windows XP SP3 (0x0501).
set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINNT=0x0501 -DWINVER=0x0501")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0501 -DWINVER=0x0501")

# Keep the .exe free of compiler-runtime DLL dependencies and mark the PE as
# XP-compatible (subsystem version 5.01).
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
