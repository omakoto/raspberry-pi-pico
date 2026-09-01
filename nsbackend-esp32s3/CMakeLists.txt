cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nsbackend-esp32s3)

# Automatically generate and flash Wear Levelling FATFS filesystem partition from fatfs_data/
fatfs_create_spiflash_image(storage fatfs_data FLASH_IN_PROJECT)
