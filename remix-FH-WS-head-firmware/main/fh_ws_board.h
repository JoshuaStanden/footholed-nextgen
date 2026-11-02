/*
 * fh_ws_board.h — Waveshare ESP32-S3-Touch-LCD-3.5 (NON-B) board adapter.
 *
 * Mirrors the PROVEN original FootHoled board bring-up
 * (JoshuaStanden/FootHoled @ josh-dev, firmware/src/board_adapter.cpp +
 * DISPLAY_NOTES.md) — bench-working on THIS exact board. Display: ST7796 over
 * STANDARD SPI (SPI2, 80 MHz). Touch: FT6336 over I2C. LCD + touch RESET lines
 * are on a TCA9554 I2C expander (NOT ESP32 GPIOs) and MUST be pulsed before
 * panel init or the screen is blank though every call returns ESP_OK.
 *
 * Registers an LVGL 9.2 display + touch indev. The render layer (fh_ws_main.c)
 * builds its UI with LVGL on top of this. C-callable (extern "C").
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up SPI + I2C + TCA9554 reset + ST7796 + FT6336 + LVGL. Returns true
 * only after the LVGL display and touch input device are registered. */
bool fh_ws_board_init(void);

/* Shared I2C0 bus (touch/IMU/RTC/PMIC all live here). NULL before init. */
i2c_master_bus_handle_t fh_ws_board_i2c_bus(void);

/* True once the LVGL display + input are registered (post init). */
bool fh_ws_board_display_ready(void);

/* Backlight 0-100 % via LEDC PWM (capped below full to avoid burn-in). */
void fh_ws_board_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
