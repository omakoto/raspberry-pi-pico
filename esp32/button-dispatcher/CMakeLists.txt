cmake_minimum_required(VERSION 3.16)

# Handle external configuration override if the ESP32_CONFIG_TOML environment variable is set (for example
# Wi-Fi credentials, kept out of git). Only the keys this firmware reads are copied into the config drive image:
# the override file is shared with other projects and may also hold their credentials (SwitchBot, Pushover,
# ...), and the drive can be read over USB.
if(DEFINED ENV{ESP32_CONFIG_TOML})
    if(EXISTS "$ENV{ESP32_CONFIG_TOML}")
        message(STATUS "Including external config override from $ENV{ESP32_CONFIG_TOML} (only the keys this firmware reads)")
        # grep exits with 1 when nothing matches, which just means the override has nothing for us.
        execute_process(
            COMMAND grep -E "^[[:space:]]*(hostname|http_port|led_pin|led_active_low|buzzer_pin|buzzer_active_low|wifi_ssid[0-9]?|wifi_password[0-9]?|ble_scan_interval_ms|ble_scan_window_ms|ble_scan_active|pbkdf2_iterations)[[:space:]]*=" "$ENV{ESP32_CONFIG_TOML}"
            OUTPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/fatfs_data/config-override.toml"
            RESULT_VARIABLE grep_result)
        # Rebuild the drive image when the override changes.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "$ENV{ESP32_CONFIG_TOML}")
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
