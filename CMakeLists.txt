cmake_minimum_required(VERSION 3.20)

project(lme2510_stream VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /EHsc /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(LME2510_BUILD_TOOLS "Build firmware embedding generator" ON)
# FTXUI v7 is cross-platform: the TUI builds on macOS/Linux and on modern
# Windows with MSVC.  MinGW/Windows-XP cross builds stay CLI-only (FTXUI is
# not validated for those toolchains there).
if(MSVC)
  set(LME2510_ENABLE_TUI_DEFAULT ON)
elseif(WIN32)
  set(LME2510_ENABLE_TUI_DEFAULT OFF)
else()
  set(LME2510_ENABLE_TUI_DEFAULT ON)
endif()
option(LME2510_ENABLE_TUI
       "Build the full-screen FTXUI TUI (macOS/Linux and MSVC Windows)"
       ${LME2510_ENABLE_TUI_DEFAULT})

# --- Firmware embedding -----------------------------------------------------
set(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fw")
set(FIRMWARE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/firmware_embedded.h")

# The firmware header generator is a plain native console program.  When
# cross-compiling it cannot be executed on the build host, so callers must
# provide a host-native copy (build it first with a native configure):
#   cmake -S . -B build/host-gen
#   cmake --build build/host-gen --target gen_firmware_embed
#   cmake ... -DLME2510_HOST_GEN_FIRMWARE_EMBED="$PWD/build/host-gen/gen_firmware_embed"
if(CMAKE_CROSSCOMPILING)
  if(NOT LME2510_HOST_GEN_FIRMWARE_EMBED OR
     NOT EXISTS "${LME2510_HOST_GEN_FIRMWARE_EMBED}")
    message(FATAL_ERROR
      "Cross-compiling requires a host-native firmware generator. Build it "
      "with a native CMake configure (target gen_firmware_embed) and pass "
      "-DLME2510_HOST_GEN_FIRMWARE_EMBED=<path>.")
  endif()
  set(GEN_FIRMWARE_EMBED_BIN "${LME2510_HOST_GEN_FIRMWARE_EMBED}")
else()
  add_executable(gen_firmware_embed tools/gen_firmware_embed.cpp)
  set(GEN_FIRMWARE_EMBED_BIN gen_firmware_embed)
endif()

set(GEN_FIRMWARE_DEPENDS
  "${FW_DIR}/fw_bootloader.bin"
  "${FW_DIR}/fw_lgs8g75.bin"
  "${FW_DIR}/fw_lgs8gl5.bin")
if(NOT CMAKE_CROSSCOMPILING)
  list(APPEND GEN_FIRMWARE_DEPENDS gen_firmware_embed)
endif()

add_custom_command(
  OUTPUT "${FIRMWARE_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${GEN_FIRMWARE_EMBED_BIN}"
          "${FIRMWARE_HEADER}"
          "${FW_DIR}/fw_bootloader.bin"  kFwBootloader
          "${FW_DIR}/fw_lgs8g75.bin"     kFwLgs8g75
          "${FW_DIR}/fw_lgs8gl5.bin"     kFwLgs8gl5
  DEPENDS ${GEN_FIRMWARE_DEPENDS}
  COMMENT "Embedding firmware blobs"
  VERBATIM)

add_custom_target(embedded_firmware DEPENDS "${FIRMWARE_HEADER}")

# --- Libraries --------------------------------------------------------------
set(LME2510_SOURCES
  src/bridge/usb_bridge.cpp
  src/i2c/i2c_bus.cpp
  src/demod/demodulator.cpp
  src/tuner/max2165.cpp
  src/frontend/receiver.cpp
  src/stream/ts_packetizer.cpp
  src/stream/ts_loss.cpp
  src/stream/program_guide.cpp
  src/stream/stream_session.cpp
  src/stream/sinks.cpp
  src/net/udp_socket.cpp
  src/util/logger.cpp
  src/util/arg_parser.cpp
  src/util/platform.cpp)

if(LME2510_ENABLE_TUI)
  list(APPEND LME2510_SOURCES
    src/tui/tui_model.cpp
    src/tui/tui_engine.cpp
    src/tui/capture_session.cpp
    src/tui/tui_app.cpp)
  # Prefer an installed FTXUI (e.g. `brew install ftxui` on macOS or the
  # vcpkg `ftxui` port on Windows); otherwise fetch the pinned release so the
  # TUI stays buildable on a clean machine.
  find_package(ftxui CONFIG QUIET)
  if(NOT ftxui_FOUND)
    message(STATUS "FTXUI not found on the system; downloading v7.0.3 source.")
    include(FetchContent)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
      FetchContent_Declare(ftxui
        URL https://github.com/ArthurSonzogni/FTXUI/releases/download/v7.0.3/source.tar.gz
        URL_HASH SHA256=be506d647bf6eed2e7927f99ea080bcccff0938d9a35617756161d5b76df8b8a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    else()
      FetchContent_Declare(ftxui
        URL https://github.com/ArthurSonzogni/FTXUI/releases/download/v7.0.3/source.tar.gz
        URL_HASH SHA256=be506d647bf6eed2e7927f99ea080bcccff0938d9a35617756161d5b76df8b8a)
    endif()
    FetchContent_MakeAvailable(ftxui)
  endif()
endif()

add_library(lme2510 STATIC ${LME2510_SOURCES})
add_dependencies(lme2510 embedded_firmware)
target_include_directories(lme2510
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(LME2510_ENABLE_TUI)
  target_link_libraries(lme2510 PUBLIC ftxui::component)
  target_compile_definitions(lme2510 PUBLIC LME2510_ENABLE_TUI=1)
endif()

# --- libusb -----------------------------------------------------------------
set(LIBUSB_INCLUDE_DIRS "")
set(LIBUSB_LIBRARIES "")

if(WIN32)
  # vcpkg: find_package(libusb CONFIG) exposes one of several target names.
  find_package(libusb CONFIG QUIET)
  if(TARGET libusb::libusb)
    set(LIBUSB_LIBRARIES libusb::libusb)
  elseif(TARGET unofficial::libusb::libusb)
    set(LIBUSB_LIBRARIES unofficial::libusb::libusb)
  else()
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h
              PATH_SUFFIXES libusb-1.0 libusb)
    find_library(LIBUSB_LIBRARY NAMES libusb-1.0 libusb)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
      message(FATAL_ERROR
        "libusb not found. Install it with vcpkg: vcpkg install libusb:x64-windows, "
        "then configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
    endif()
    set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_LIBRARIES "${LIBUSB_LIBRARY}")
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    # pkg_check_modules caches its result in the cache; force a re-check on
    # every configure so its imported target is always recreated.
    unset(LIBUSB_FOUND CACHE)
    unset(__pkg_config_checked_LIBUSB CACHE)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
  endif()
  if(TARGET PkgConfig::LIBUSB)
    set(LIBUSB_LIBRARIES PkgConfig::LIBUSB)
  else()
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h
              PATH_SUFFIXES libusb-1.0 libusb)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
      message(FATAL_ERROR
        "libusb-1.0 not found. macOS: brew install libusb pkg-config | "
        "Linux: apt install libusb-1.0-0-dev pkg-config")
    endif()
    set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_LIBRARIES "${LIBUSB_LIBRARY}")
  endif()
endif()

target_link_libraries(lme2510 PUBLIC ${LIBUSB_LIBRARIES})
target_include_directories(lme2510 PUBLIC ${LIBUSB_INCLUDE_DIRS})
if(LIBUSB_LIBRARY_DIRS)
  target_link_directories(lme2510 PUBLIC ${LIBUSB_LIBRARY_DIRS})
endif()
if(WIN32)
  target_link_libraries(lme2510 PUBLIC ws2_32)
  target_compile_definitions(lme2510 PUBLIC NOMINMAX)
endif()

# --- Executable -------------------------------------------------------------
add_executable(lme2510_stream src/main.cpp)
target_link_libraries(lme2510_stream PRIVATE lme2510)

# --- Packaging / convenient targets ------------------------------------------
install(TARGETS lme2510_stream RUNTIME DESTINATION bin)
install(TARGETS lme2510 ARCHIVE DESTINATION lib)
install(DIRECTORY include/lme2510 DESTINATION include)

add_custom_target(run_help
  COMMAND lme2510_stream --help
  DEPENDS lme2510_stream)
