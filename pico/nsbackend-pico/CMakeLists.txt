cmake_minimum_required(VERSION 3.16)

# Copy external configuration file if PICO_CONFIG_TOML or ESP32_CONFIG_TOML is set
if(DEFINED ENV{PICO_CONFIG_TOML})
    if(EXISTS "$ENV{PICO_CONFIG_TOML}")
        message(STATUS "Including external config override from $ENV{PICO_CONFIG_TOML}")
        configure_file("$ENV{PICO_CONFIG_TOML}" "${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data/config-override.toml" COPYONLY)
    endif()
elseif(DEFINED ENV{ESP32_CONFIG_TOML})
    if(EXISTS "$ENV{ESP32_CONFIG_TOML}")
        message(STATUS "Including external config override from $ENV{ESP32_CONFIG_TOML}")
        configure_file("$ENV{ESP32_CONFIG_TOML}" "${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data/config-override.toml" COPYONLY)
    endif()
endif()

# Default board to pico_w if not specified
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w)
endif()

# Setup Pico SDK path
set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(nsbackend-pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# Setup FreeRTOS Kernel path and import SMP port
set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
if(NOT FREERTOS_KERNEL_PATH)
    set(FREERTOS_KERNEL_PATH "$ENV{HOME}/FreeRTOS-Kernel")
endif()
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

# Determine if Wi-Fi is supported on current board
if(PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    set(NSBACKEND_HAS_WIFI 1)
    message(STATUS "Building nsbackend-pico with Wi-Fi support enabled for board: ${PICO_BOARD}")
else()
    set(NSBACKEND_HAS_WIFI 0)
    message(STATUS "Building nsbackend-pico without Wi-Fi support for board: ${PICO_BOARD}")
endif()

# Setup FatFs with custom configuration
set(FATFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/fatfs")
file(MAKE_DIRECTORY "${FATFS_DIR}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/ffconf.h" "${FATFS_DIR}/ffconf.h" COPYONLY)
configure_file("${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ff.c" "${FATFS_DIR}/ff.c" COPYONLY)
configure_file("${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ff.h" "${FATFS_DIR}/ff.h" COPYONLY)
configure_file("${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/ffunicode.c" "${FATFS_DIR}/ffunicode.c" COPYONLY)
configure_file("${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source/diskio.h" "${FATFS_DIR}/diskio.h" COPYONLY)

set(APP_SOURCES
    src/main.cpp
    src/flash_fatfs.cpp
    src/config_manager.cpp
    src/gamepad_hid.cpp
    src/controller_state.cpp
    src/gpio_buttons.cpp
    src/i2c_keypad.cpp
    src/status_led.cpp
    src/dual_logger.cpp
    src/serial_command_server.cpp
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffunicode.c
)

if(NSBACKEND_HAS_WIFI)
    list(APPEND APP_SOURCES
        src/wifi_manager.cpp
        src/mdns_service.cpp
        src/tcp_server.cpp
        ${PICO_SDK_PATH}/lib/lwip/src/apps/mdns/mdns.c
        ${PICO_SDK_PATH}/lib/lwip/src/apps/mdns/mdns_domain.c
        ${PICO_SDK_PATH}/lib/lwip/src/apps/mdns/mdns_out.c
    )
endif()

add_executable(nsbackend-pico ${APP_SOURCES})

target_compile_definitions(nsbackend-pico PRIVATE
    NSBACKEND_HAS_WIFI=${NSBACKEND_HAS_WIFI}
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_fat_image.py
        --input_dir ${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data
        --header_file ${CMAKE_CURRENT_BINARY_DIR}/generated/default_configs.hpp
    RESULT_VARIABLE CONFIG_GEN_RES
)

target_include_directories(nsbackend-pico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${FATFS_DIR}
)

target_link_libraries(nsbackend-pico PRIVATE
    pico_stdlib
    tinyusb_device
    tinyusb_board
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    hardware_flash
    hardware_sync
    hardware_uart
    hardware_gpio
    hardware_i2c
    hardware_timer
)

if(NSBACKEND_HAS_WIFI)
    target_link_libraries(nsbackend-pico PRIVATE
        pico_cyw43_arch_lwip_sys_freertos
    )
endif()

pico_enable_stdio_uart(nsbackend-pico 0)
pico_enable_stdio_usb(nsbackend-pico 0)

pico_add_extra_outputs(nsbackend-pico)

# Target to generate storage.bin and storage.uf2
add_custom_target(storage
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_fat_image.py
        --input_dir ${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data
        --output_file ${CMAKE_BINARY_DIR}/storage.bin
        --uf2_file ${CMAKE_BINARY_DIR}/storage.uf2
        --uf2_offset 0x10100000
        --header_file ${CMAKE_CURRENT_BINARY_DIR}/generated/default_configs.hpp
    COMMENT "Generating 1MB FAT partition image storage.bin, storage.uf2, and default_configs.hpp"
    VERBATIM
)

add_dependencies(nsbackend-pico storage)

# Merge firmware UF2 and FAT storage UF2 into single combined nsbackend-pico.uf2
add_custom_command(TARGET nsbackend-pico POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_fat_image.py
        --combine_uf2
        --firmware_uf2 ${CMAKE_BINARY_DIR}/nsbackend-pico.uf2
        --storage_uf2 ${CMAKE_BINARY_DIR}/storage.uf2
        --output_uf2 ${CMAKE_BINARY_DIR}/nsbackend-pico.uf2
    COMMENT "Combining firmware and FAT storage partition into single nsbackend-pico.uf2"
    VERBATIM
)
