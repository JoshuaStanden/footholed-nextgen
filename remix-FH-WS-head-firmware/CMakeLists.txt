# FHAdvanced-REMIX Waveshare HEAD — top-level project CMake (ESP-IDF 6.1).
# Board: Waveshare ESP32-S3-Touch-LCD-3.5.
# Build:            idf.py set-target esp32s3 && idf.py build
# Flash + monitor:  idf.py -p <port> flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remix_fh_ws_head)
