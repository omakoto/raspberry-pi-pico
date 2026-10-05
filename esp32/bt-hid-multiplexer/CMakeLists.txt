cmake_minimum_required(VERSION 3.16)

# Target board, selected with ./00-build.sh -b <devkitc|xiao>. Only the on-board LED driver and the
# board name differ; every external I/O uses the same GPIOs on both boards (see DESIGN.md §2).
set(BOARD "devkitc" CACHE STRING "Target board: devkitc (ESP32-S3-DevKitC-1) or xiao (Seeed Studio XIAO ESP32-S3)")
set_property(CACHE BOARD PROPERTY STRINGS devkitc xiao)
if(NOT BOARD STREQUAL "devkitc" AND NOT BOARD STREQUAL "xiao")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'; use devkitc or xiao")
endif()

# For development: -DUSB_SERIAL_ALWAYS=ON always includes the USB serial port, without grounding
# PIN_USB_SERIAL_ENABLE.
option(USB_SERIAL_ALWAYS "Always enable the USB serial port" OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

string(TOUPPER "${BOARD}" BOARD_UPPER)
idf_build_set_property(COMPILE_DEFINITIONS "BOARD_${BOARD_UPPER}=1" APPEND)
if(USB_SERIAL_ALWAYS)
    idf_build_set_property(COMPILE_DEFINITIONS "USB_SERIAL_ALWAYS=1" APPEND)
endif()

# TinyUSB arms the HID OUT endpoint for CFG_TUD_HID_EP_BUFSIZE bytes and a transfer only completes at
# a short packet or when the buffer is full. The VIAL raw HID endpoint is 32 bytes, so with a larger
# buffer every 32-byte VIAL request would stay pending until the next one arrives. esp_tinyusb does
# not set this value (TinyUSB's default is 64), and TinyUSB only defines its default when the value is
# not set already, so it is set for every component here.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_HID_EP_BUFSIZE=32" APPEND)

project(bt-hid-multiplexer)
