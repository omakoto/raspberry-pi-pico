cmake_minimum_required(VERSION 3.16)

# Handle external configuration override if the ESP32_CONFIG_TOML environment variable is set (for example
# Wi-Fi credentials, kept out of git). The whole file is copied into the config drive image: it is shared with
# other projects and may hold their credentials too (SwitchBot, Pushover, ...), but the firmware moves every
# secret (keys ending in _password, _key or _token) into private storage and comments it out in the file
# before the drive is first shown over USB. See "Configuration" in the README.
if(DEFINED ENV{ESP32_CONFIG_TOML})
    if(EXISTS "$ENV{ESP32_CONFIG_TOML}")
        message(STATUS "Including external config override from $ENV{ESP32_CONFIG_TOML}")
        configure_file("$ENV{ESP32_CONFIG_TOML}" "${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data/config-override.toml" COPYONLY)
    else()
        message(WARNING "ESP32_CONFIG_TOML is set to '$ENV{ESP32_CONFIG_TOML}', but the file does not exist.")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(button-dispatcher)

# Generate and flash the wear-levelling FATFS image of the config drive (config.toml) from fatfs_data/.
# The `state` partition (settings, bindings, password hash) is deliberately not flashed, so reflashing
# the firmware keeps what the user configured on the device.
fatfs_create_spiflash_image(storage fatfs_data FLASH_IN_PROJECT)
