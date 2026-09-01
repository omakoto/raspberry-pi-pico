cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nsbackend-esp32s3)

# Automatically generate and flash SPIFFS filesystem partition from spiffs_data/
spiffs_create_partition_image(storage spiffs_data FLASH_IN_PROJECT)
