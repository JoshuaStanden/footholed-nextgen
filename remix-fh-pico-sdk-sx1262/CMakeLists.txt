cmake_minimum_required(VERSION 3.13)

# Pico SDK import
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(fh_pico_sx1262 C CXX ASM)

# No host C/C++ compiler on this machine -> picotool cannot be built from
# source. The .uf2 is made from the .bin with uf2conv.py (see README), exactly
# like the GPS/cc1101 blades. Keeps elf/bin/hex/dis outputs.
set(PICO_NO_PICOTOOL 1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- RadioLib (the vetted upstream LoRa library) --------------------------- #
# RadioLib is NOT vendored into this package (it is large and actively
# maintained). Get it and pin the tag (one-time):
#   git clone https://github.com/jgromes/RadioLib.git
#   cd RadioLib && git checkout 7.8.1
# Then point RADIOLIB_PATH at that checkout, e.g.:
#   cmake -B build -S . -DRADIOLIB_PATH=/path/to/RadioLib ...
# PIN 7.8.1 (HARD): sx_shim.cpp uses the positional SX1262::begin() removed on
# 8.x; an 8.x build trips an #error in sx_shim.cpp. The NonArduino Pico HAL
# lives at ${RADIOLIB_PATH}/src/hal/RPiPico/PicoHal.{h,cpp}.
if(NOT DEFINED RADIOLIB_PATH)
    message(FATAL_ERROR
        "Set -DRADIOLIB_PATH=/path/to/RadioLib (pinned tag, e.g. 7.8.1). "
        "See README. The Pico HAL is at src/hal/RPiPico/PicoHal.cpp.")
endif()
add_subdirectory(${RADIOLIB_PATH} ${CMAKE_CURRENT_BINARY_DIR}/RadioLib)

add_executable(fh_pico_sx1262
    main.c                 # C: hub pump (spi0) + core1 LoRa executor
    fh_frame.c             # byte-identical shared protocol
    newlib_lock_stubs.c
    sx_shim.cpp            # C++: extern "C" bridge over RadioLib + PicoHal (spi1)
    ${RADIOLIB_PATH}/src/hal/RPiPico/PicoHal.cpp   # RadioLib's Pico-SDK HAL
)

# RadioLib's PicoHal + headers compile only behind this define.
target_compile_definitions(fh_pico_sx1262 PUBLIC RADIOLIB_BUILD_RPI_PICO)

target_include_directories(fh_pico_sx1262 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${RADIOLIB_PATH}/src          # so sx_shim.cpp can #include "hal/RPiPico/PicoHal.h"
)

target_link_libraries(fh_pico_sx1262
    pico_stdlib
    pico_multicore      # core1 radio executor + PicoHal's multicore use
    hardware_spi        # spi0 (hub link) + spi1 (radio)
    hardware_gpio
    hardware_timer      # PicoHal
    hardware_pwm        # PicoHal tone() impl (unused but compiled)
    hardware_resets     # hub pump full-reset per transaction
    RadioLib
)

# NO USB stdio (keeps the build free of tinyusb submodules), same as GPS.
pico_enable_stdio_usb(fh_pico_sx1262 0)
pico_enable_stdio_uart(fh_pico_sx1262 0)

# Optional debug UART0 (GP0/GP1 @ 115200). Default OFF.
if(FH_DEBUG_UART)
    target_compile_definitions(fh_pico_sx1262 PRIVATE FH_DEBUG_UART=1)
endif()

pico_add_extra_outputs(fh_pico_sx1262)
