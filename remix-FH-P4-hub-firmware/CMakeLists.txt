# FHAdvanced-REMIX HUB — top-level project CMake (ESP-IDF 6.1).
# Build:            idf.py set-target esp32p4 && idf.py build
# Flash + monitor:  idf.py -p <port> flash monitor      (over USB-C / USB-JTAG)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remix_fh_p4_hub)
