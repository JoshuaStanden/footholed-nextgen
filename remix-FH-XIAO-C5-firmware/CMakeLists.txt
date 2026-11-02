# FHAdvanced-REMIX XIAO-C5 — top-level project CMake (ESP-IDF 6.1).
# Build:            idf.py set-target esp32c5 && idf.py build
# Flash + monitor:  idf.py -p <port> flash monitor   (USB-C / USB-Serial-JTAG)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remix_fh_xiao_c5)
