cmake_minimum_required(VERSION 3.16)

# Copy external configuration file if ESP32_CONFIG_TOML environment variable is set
if(DEFINED ENV{ESP32_CONFIG_TOML})
    if(EXISTS "$ENV{ESP32_CONFIG_TOML}")
        message(STATUS "Including external config override from $ENV{ESP32_CONFIG_TOML}")
        configure_file("$ENV{ESP32_CONFIG_TOML}" "${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data/config-override.toml" COPYONLY)
    else()
        message(WARNING "ESP32_CONFIG_TOML is set to '$ENV{ESP32_CONFIG_TOML}', but the file does not exist.")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nsbackend-esp32s3)

# Automatically generate and flash Wear Levelling FATFS filesystem partition from fatfs_data/
fatfs_create_spiflash_image(storage fatfs_data FLASH_IN_PROJECT)

