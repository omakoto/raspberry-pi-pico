cmake_minimum_required(VERSION 3.16)

# For development: -DUSB_SERIAL_ALWAYS=ON always includes the USB serial port, without grounding
# PIN_USB_SERIAL_ENABLE.
option(USB_SERIAL_ALWAYS "Always enable the USB serial port" OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# TinyUSB arms the HID OUT endpoint for CFG_TUD_HID_EP_BUFSIZE bytes and a transfer only completes at
# a short packet or when the buffer is full. The VIAL raw HID endpoint is 32 bytes, so with a larger
# buffer every 32-byte VIAL request would stay pending until the next one arrives. esp_tinyusb does
# not set this value (TinyUSB's default is 64), and TinyUSB only defines its default when the value is
# not set already, so it is set for every component here.
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_HID_EP_BUFSIZE=32" APPEND)

project(bt-hid-multiplexer)

# The USB serial option only matters to this firmware's own code; defining it for main alone keeps
# switching it from rebuilding all of ESP-IDF.
idf_component_get_property(main_lib main COMPONENT_LIB)
if(USB_SERIAL_ALWAYS)
    target_compile_definitions(${main_lib} PRIVATE USB_SERIAL_ALWAYS=1)
endif()
