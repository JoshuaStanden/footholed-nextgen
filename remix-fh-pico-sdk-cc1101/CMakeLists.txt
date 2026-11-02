cmake_minimum_required(VERSION 3.13)

# Pico SDK import
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(fh_pico_cc1101 C CXX ASM)

# No host C/C++ compiler on this machine -> picotool cannot be built from
# source. The .uf2 is generated from the .bin with uf2conv.py (see README),
# exactly like the GPS blade. PICO_NO_PICOTOOL disables picotool post-processing
# while keeping elf/bin/hex/dis outputs.
set(PICO_NO_PICOTOOL 1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(fh_pico_cc1101
    main.c
    fh_frame.c
    newlib_lock_stubs.c
    ELECHOUSE_CC1101_SRC_DRV.cpp   # vetted SDK CC1101 driver (spi1 master)
    cc_shim.cpp                    # extern "C" bridge: C main.c -> C++ driver + OOK TX
)

target_include_directories(fh_pico_cc1101 PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(fh_pico_cc1101
    pico_stdlib
    pico_multicore
    hardware_spi      # spi0 (hub link) + spi1 (radio)
    hardware_gpio
    hardware_timer
    hardware_irq
    hardware_resets
)

# NO USB stdio (keeps the build free of tinyusb submodules), same as GPS.
pico_enable_stdio_usb(fh_pico_cc1101 0)
pico_enable_stdio_uart(fh_pico_cc1101 0)

# Optional debug UART0 (GP0/GP1 @ 115200). Default OFF.
if(FH_DEBUG_UART)
    target_compile_definitions(fh_pico_cc1101 PRIVATE FH_DEBUG_UART=1)
endif()

pico_add_extra_outputs(fh_pico_cc1101)
