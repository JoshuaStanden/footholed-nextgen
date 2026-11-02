cmake_minimum_required(VERSION 3.13)

# Pico SDK import
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(fh_pico_gps C CXX ASM)

# No host C/C++ compiler on this machine -> picotool cannot be built from
# source. The .uf2 is generated from the .bin with the Arduino rp2040 core's
# uf2conv.py instead (see README). PICO_NO_PICOTOOL disables picotool
# post-processing (uf2/sign/coprodis) while keeping elf/bin/hex/dis outputs.
set(PICO_NO_PICOTOOL 1)   # this picotool build rejects --platform; we convert the .bin manually

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(fh_pico_gps
    main.c
    fh_frame.c
    newlib_lock_stubs.c
)

target_include_directories(fh_pico_gps PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(fh_pico_gps
    pico_stdlib
    pico_multicore
    hardware_uart
    hardware_spi
)

# NO USB stdio (keeps the build free of tinyusb submodules).
pico_enable_stdio_usb(fh_pico_gps 0)
pico_enable_stdio_uart(fh_pico_gps 0)

# Optional debug UART0 (GP0/GP1 @ 115200). Default OFF.
if(FH_DEBUG_UART)
    target_compile_definitions(fh_pico_gps PRIVATE FH_DEBUG_UART=1)
endif()

# Optional indoor-bench GPS simulation (bypasses UART, injects a static fix).
# Default OFF — never ship a real rig with this on.
if(FH_GPS_SIM)
    target_compile_definitions(fh_pico_gps PRIVATE FH_GPS_SIM=1)
endif()

pico_add_extra_outputs(fh_pico_gps)

# BENCH: bare-metal slave isolation test (slave_min.c). Build with:
#   cmake --build build --target slave_min
add_executable(slave_min
    slave_min.c
)
target_link_libraries(slave_min pico_stdlib hardware_spi)
pico_enable_stdio_usb(slave_min 0)
pico_enable_stdio_uart(slave_min 0)
pico_add_extra_outputs(slave_min)
