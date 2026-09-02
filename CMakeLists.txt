cmake_minimum_required(VERSION 3.20)

project(lme2510_stream VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(LME2510_BUILD_TOOLS "Build firmware embedding generator" ON)

# --- Firmware embedding -----------------------------------------------------
set(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fw")
set(FIRMWARE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/firmware_embedded.h")

add_executable(gen_firmware_embed tools/gen_firmware_embed.cpp)

add_custom_command(
  OUTPUT "${FIRMWARE_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND gen_firmware_embed
          "${FIRMWARE_HEADER}"
          "${FW_DIR}/fw_bootloader.bin"  kFwBootloader
          "${FW_DIR}/fw_lgs8g75.bin"     kFwLgs8g75
          "${FW_DIR}/fw_lgs8gl5.bin"     kFwLgs8gl5
  DEPENDS
    gen_firmware_embed
    "${FW_DIR}/fw_bootloader.bin"
    "${FW_DIR}/fw_lgs8g75.bin"
    "${FW_DIR}/fw_lgs8gl5.bin"
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
  src/stream/stream_session.cpp
  src/stream/sinks.cpp
  src/net/udp_socket.cpp
  src/util/logger.cpp
  src/util/arg_parser.cpp
  src/util/platform.cpp)

add_library(lme2510 STATIC ${LME2510_SOURCES})
add_dependencies(lme2510 embedded_firmware)
target_include_directories(lme2510
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

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
    pkg_check_modules(LIBUSB libusb-1.0)
  endif()
  if(NOT LIBUSB_FOUND)
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
