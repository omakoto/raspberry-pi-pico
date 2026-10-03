cmake_minimum_required(VERSION 3.16)

# Default target board to pico2_w (Raspberry Pi Pico 2 W with RP2350 + CYW43439)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

# Setup Pico SDK path
set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(bt-hid-multiplexer C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

set(APP_SOURCES
    src/main.cpp
    src/usb_descriptors.c
    src/ssd1306.cpp
    src/button_handler.cpp
    src/virtual_matrix.cpp
    src/vial_server.cpp
    src/storage.cpp
    src/multiplexer.cpp
    src/ble_hid_host.cpp
    src/dual_console.cpp
)

add_executable(bt-hid-multiplexer ${APP_SOURCES})

# Target include directories
target_include_directories(bt-hid-multiplexer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compiler warnings and options
target_compile_options(bt-hid-multiplexer PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_compile_definitions(bt-hid-multiplexer PRIVATE
    CYW43_LWIP=0
    PICO_FLASH_ASSUME_CORE1_SAFE=1
)

# HID-over-GATT diagnostics: with ENABLE_TESTING_SUPPORT the BTstack HIDS client prints its
# whole discovery on the console (HID characteristics, Report Reference IDs/types, CCCD handles,
# each notification enable) and 'getreport' reads back the CCCD before the report. This is the
# only way to see why a peripheral that pairs and encrypts fine never delivers input reports.
# The define must stay per-source: globally it also widens btstack_time_t to 64 bits, which
# breaks the run loop build. It is applied to btstack_memory.c as well because the define adds a
# field to hids_client_t, and the memory pool must allocate entries of the size hids_client.c uses.
set_source_files_properties(
    ${PICO_SDK_PATH}/lib/btstack/src/ble/gatt-service/hids_client.c
    ${PICO_SDK_PATH}/lib/btstack/src/btstack_memory.c
    PROPERTIES COMPILE_DEFINITIONS ENABLE_TESTING_SUPPORT
)

# Link essential Pico SDK and driver libraries
target_link_libraries(bt-hid-multiplexer PRIVATE
    pico_stdlib
    pico_btstack_ble
    pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background
    tinyusb_device
    tinyusb_board
    hardware_i2c
    hardware_gpio
    hardware_flash
    hardware_sync
    hardware_timer
    hardware_uart
    pico_bootrom
    pico_unique_id
)

pico_set_program_name(bt-hid-multiplexer "Pico 2 W BLE HID Multiplexer")
pico_set_program_version(bt-hid-multiplexer "1.0.0")

# Generate UF2, HEX, and map files
pico_add_extra_outputs(bt-hid-multiplexer)

# Route printf output to TinyUSB CDC serial port
pico_enable_stdio_usb(bt-hid-multiplexer 0)
pico_enable_stdio_uart(bt-hid-multiplexer 0)
