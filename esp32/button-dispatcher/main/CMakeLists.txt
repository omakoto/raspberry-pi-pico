idf_component_register(
    SRCS
        "main.cpp"
        "config_manager.cpp"
        "status_led.cpp"
        "usb_msc.cpp"
        "dual_logger.cpp"
        "wifi_manager.cpp"
        "mdns_service.cpp"
        "web_server.cpp"
        "esp_platform.cpp"
        "ble_scanner.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        dispatcher_core
        esp_netif
        esp_event
        esp_timer
        esp_wifi
        esp_http_server
        esp_http_client
        esp-tls
        mbedtls
        json
        bt
        driver
        lwip
        freertos
        log
        nvs_flash
        fatfs
        wear_levelling
        esp_partition
        mdns
        esp_tinyusb
)

# The web page is the one the Raspberry Pi version serves (pi/button-dispatcher/.../static), so there is a
# single copy of the UI. Its files are gzip-compressed into the build directory and embedded in the
# firmware; web_server.cpp sends them with "Content-Encoding: gzip".
set(WEB_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../pi/button-dispatcher/button_dispatcher/static")
set(WEB_GZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/web")
file(MAKE_DIRECTORY "${WEB_GZIP_DIR}")
foreach(web_file index.html app.js style.css favicon.svg)
    set(web_source "${WEB_SOURCE_DIR}/${web_file}")
    set(web_gzip "${WEB_GZIP_DIR}/${web_file}.gz")
    add_custom_command(
        OUTPUT "${web_gzip}"
        COMMAND gzip -9 -n -c "${web_source}" > "${web_gzip}"
        DEPENDS "${web_source}"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${web_gzip}" BINARY)
endforeach()
