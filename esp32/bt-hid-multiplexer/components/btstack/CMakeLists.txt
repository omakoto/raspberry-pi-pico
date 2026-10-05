# BTstack as a project-local component.
#
# BTstack's own port/esp32/integrate_btstack.py copies BTstack into $IDF_PATH/components, which changes
# the shared ESP-IDF tree, and its stock include/btstack_config.h would be on the include path. This
# component instead compiles the sources straight from a BTstack tree (by default the one inside the
# Pico SDK, i.e. the same BTstack version the Pico build of this firmware uses) and provides its own
# btstack_config.h. Only what a BLE HID host needs is compiled: no Classic, mesh, audio or lwIP.
#
# port/ holds a copy of the ESP32 port glue (port/esp32/components/btstack of the same BTstack
# version); it is copied because its include directory also contains the stock btstack_config.h.

if(DEFINED ENV{BTSTACK_ROOT})
    set(BTSTACK_ROOT "$ENV{BTSTACK_ROOT}")
else()
    set(BTSTACK_ROOT "$ENV{HOME}/pico-sdk/lib/btstack")
endif()
if(NOT EXISTS "${BTSTACK_ROOT}/src/btstack.h")
    message(FATAL_ERROR "BTstack not found at '${BTSTACK_ROOT}'. Set BTSTACK_ROOT to a BTstack tree.")
endif()

set(srcs
    src/ad_parser.c
    src/btstack_crypto.c
    src/btstack_hid.c
    src/btstack_hid_parser.c
    src/btstack_linked_list.c
    src/btstack_memory.c
    src/btstack_memory_pool.c
    src/btstack_ring_buffer.c
    src/btstack_run_loop.c
    src/btstack_run_loop_base.c
    src/btstack_tlv.c
    src/btstack_util.c
    src/hci.c
    src/hci_cmd.c
    src/hci_dump.c
    src/hci_event.c
    src/hci_event_builder.c
    src/l2cap.c
    src/l2cap_signaling.c
    src/ble/att_db.c
    src/ble/att_db_util.c
    src/ble/att_dispatch.c
    src/ble/att_server.c
    src/ble/gatt_client.c
    src/ble/gatt_service_client.c
    src/ble/le_device_db_tlv.c
    src/ble/sm.c
    src/ble/gatt-service/hids_client.c
    3rd-party/micro-ecc/uECC.c
    3rd-party/rijndael/rijndael.c
    platform/freertos/btstack_run_loop_freertos.c
    platform/embedded/hci_dump_embedded_stdout.c
)
list(TRANSFORM srcs PREPEND "${BTSTACK_ROOT}/")

idf_component_register(
    SRCS
        ${srcs}
        "port/btstack_port_esp32.c"
        "port/btstack_tlv_esp32.c"
    INCLUDE_DIRS
        "include"
        "port"
        "${BTSTACK_ROOT}/src"
        "${BTSTACK_ROOT}/src/ble"
        "${BTSTACK_ROOT}/platform/freertos"
        "${BTSTACK_ROOT}/platform/embedded"
        "${BTSTACK_ROOT}/3rd-party/micro-ecc"
        "${BTSTACK_ROOT}/3rd-party/rijndael"
    REQUIRES
        bt
        nvs_flash
        freertos
)

# BTstack's own code is not warning-clean under ESP-IDF's -Werror defaults.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-variable -Wno-unused-function)
