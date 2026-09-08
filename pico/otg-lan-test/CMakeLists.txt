cmake_minimum_required(VERSION 3.16)

# otg-lan-test: makes the Pico's native USB port appear to the host PC as a USB Ethernet
# adapter (RNDIS or CDC-ECM), then runs a DHCP server and a tiny HTTP server on it, in the
# same spirit as GP2040-CE's web configurator mode. A CDC serial console rides along for
# logs and for rebooting into BOOTSEL from the host. See README.md.

# Default board to pico (RP2040, non-W) if not specified
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico)
endif()

# Setup Pico SDK path
set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(otg-lan-test C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(TINYUSB_NETWORKING_DIR "${PICO_SDK_PATH}/lib/tinyusb/lib/networking")

add_executable(otg-lan-test
    src/main.c
    src/usb_descriptors.c
    # DHCP server that ships with TinyUSB (lwIP raw API, no OS needed).
    ${TINYUSB_NETWORKING_DIR}/dhserver.c
    # RNDIS control-message handler; ecm_rndis_device.c calls into it but the
    # SDK's tinyusb_device library does not compile it, so we have to.
    ${TINYUSB_NETWORKING_DIR}/rndis_reports.c
)

# These third-party files are not warning-clean under the SDK's default flags.
set_source_files_properties(
    ${TINYUSB_NETWORKING_DIR}/dhserver.c
    ${TINYUSB_NETWORKING_DIR}/rndis_reports.c
    PROPERTIES COMPILE_FLAGS "-Wno-conversion -Wno-sign-conversion -Wno-unused-parameter"
)

target_include_directories(otg-lan-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # tusb_config.h, lwipopts.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TINYUSB_NETWORKING_DIR}       # dhserver.h
)

# Bake fs/ into the firmware as lwIP httpd content (generates pico_fsdata.inc).
pico_set_lwip_httpd_content(otg-lan-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fs/index.html
)

target_compile_definitions(otg-lan-test PRIVATE
    # pico_stdio_usb turns these off by default when the application owns TinyUSB (we do,
    # via tinyusb_device), because they need matching entries in the descriptors. Ours
    # has them, see usb_descriptors.c.
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1        # host sets 1200 baud -> BOOTSEL
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1 # picotool reboot -f -u
    # The MS OS 2.0 descriptor would only serve to bind WinUSB to the reset interface on
    # Windows, and it requires the BOS descriptor + bcdUSB 2.1; not needed here.
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR=0
)

target_link_libraries(otg-lan-test PRIVATE
    pico_stdlib
    pico_unique_id
    tinyusb_device
    pico_stdio_usb     # CDC console + 1200-baud reset + picotool reset interface
    pico_lwip          # lwIP core, NO_SYS=1 (see lwipopts.h); we drive it by polling
    pico_lwip_arch     # arch/cc.h for the Pico
    pico_lwip_http     # httpd + fs
)

# Logs go to both the USB CDC console and UART0 (GP0 TX / GP1 RX, 115200 baud).
# Note: since this project links tinyusb_device itself, pico_stdio_usb does not start
# its own background tud_task(); the main loop in main.c polls it.
pico_enable_stdio_uart(otg-lan-test 1)
pico_enable_stdio_usb(otg-lan-test 1)

pico_add_extra_outputs(otg-lan-test)
