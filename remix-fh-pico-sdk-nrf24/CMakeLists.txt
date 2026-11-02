cmake_minimum_required(VERSION 3.13)

# Pico SDK import
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(fh_pico_nrf24 C CXX ASM)

# No host C/C++ compiler on this machine -> picotool cannot be built from
# source. The .uf2 is made from the .bin with uf2conv.py (see README), exactly
# like the GPS/cc1101 blades. Keeps elf/bin/hex/dis outputs.
set(PICO_NO_PICOTOOL 1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# All C — the nRF24 driver (nrf_shim.c) is FH-owned, not a vendored library,
# so there is NO external dependency and NO C/C++ bridge (contrast sx1262's
# RadioLib shim). Simplest of the three radio blades to build.
add_executable(fh_pico_nrf24
    main.c
    nrf_shim.c         # FH-owned minimal nRF24L01+ driver (spi1 master)
    fh_frame.c
    newlib_lock_stubs.c
)

target_include_directories(fh_pico_nrf24 PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(fh_pico_nrf24
    pico_stdlib
    pico_multicore
    hardware_spi       # spi0 (hub link) + spi1 (radio)
    hardware_gpio
    hardware_resets
)

# NO USB stdio (keeps the build free of tinyusb submodules), same as GPS.
pico_enable_stdio_usb(fh_pico_nrf24 0)
pico_enable_stdio_uart(fh_pico_nrf24 0)

# Optional debug UART0 (GP0/GP1 @ 115200). Default OFF.
if(FH_DEBUG_UART)
    target_compile_definitions(fh_pico_nrf24 PRIVATE FH_DEBUG_UART=1)
endif()

pico_add_extra_outputs(fh_pico_nrf24)
