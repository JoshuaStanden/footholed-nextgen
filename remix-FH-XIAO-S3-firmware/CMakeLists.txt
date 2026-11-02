# FHAdvanced-REMIX XIAO-S3 — top-level project CMake (ESP-IDF 6.1).
# Build:            idf.py set-target esp32s3 && idf.py build
# Flash + monitor:  idf.py -p <port> flash monitor   (USB-C / USB-Serial-JTAG)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remix_fh_xiao_s3)
