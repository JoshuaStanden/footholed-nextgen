/*
 * fh_ws_pins.h — FHAdvanced-REMIX Waveshare HEAD pin map.
 * Board: Waveshare ESP32-S3-Touch-LCD-3.5 (NON-B).
 *
 * ##########################################################################
 * # TWO PIN DOMAINS on the head:                                            #
 * #                                                                          #
 * # 1. DISPLAY / TOUCH / POWER — OWNED BY THE PROVEN BOARD ADAPTER           #
 * #    (fh_ws_board.cpp). Those pins are FIXED by the hardware and the       #
 * #    original-FootHoled bring-up: LCD on SPI2 (MOSI=GPIO1, SCLK=GPIO5,     #
 * #    DC=GPIO3, CS/RST=NC-on-TCA9554, BL=GPIO6 LEDC); shared I2C0 SDA=GPIO8 #
 * #    SCL=GPIO7 (touch FT6336 + TCA9554 + IMU/RTC/PMIC). Defined inside the #
 * #    adapter, NOT here, so there is one source of truth for the proven     #
 * #    values. Do not duplicate or "improve" them.                           #
 * #                                                                          #
 * # 2. BUS A — the REMIX SPI-Slave-HD link to the P4 hub. NEW wiring on the  #
 * #    2.54 mm expansion header. These are PLACEHOLDERS — VERIFY each is a   #
 * #    free header GPIO on your board (NOT one of the display/I2C pins        #
 * #    above, and not a strapping pin).                                      #
 * ##########################################################################
 *
 * ★ R-SPIHOST (RESOLVED): the ESP32-S3 has two general SPI hosts, SPI2 + SPI3.
 *   The PROVEN LCD adapter uses SPI2_HOST. So the Bus-A HD SLAVE MUST use SPI3
 *   (FH_WS_BUSA_SPI_HOST = SPI3_HOST below). They cannot share a host (one is a
 *   master, one a slave-HD). The LCD keeps SPI2 (don't touch the proven path);
 *   Bus A takes SPI3.
 */
#pragma once
#include "driver/spi_common.h"   /* SPI3_HOST */

/* ===========================================================================
 * BUS A — SPI Slave HD link to the P4 hub (quad, committed — see
 * ../../remix-BUS-A-QUAD-HD-DECISION.md). ON SPI3 (SPI2 is the LCD).
 * All GPIOs below are PLACEHOLDERS on the expansion header — VERIFY each is
 * free on your board and not a display/I2C/strapping pin.
 * ======================================================================== */
#define FH_WS_BUSA_SPI_HOST  SPI3_HOST    /* RESOLVED: SPI2 is the LCD        */

#define FH_WS_SPI_SCK     12   /* Bus-A SCK  (hub drives)        [VERIFY]      */
#define FH_WS_SPI_MISO    13   /* Bus-A MISO/DQ1 (head -> hub)   [VERIFY]      */
#define FH_WS_SPI_MOSI    11   /* Bus-A MOSI/DQ0 (hub -> head)   [VERIFY]      */
#define FH_WS_SPI_CS      10   /* Bus-A CS   (hub selects)       [VERIFY]      */
#define FH_WS_SPI_WP      14   /* Bus-A DQ2 (QUADWP, QIO)        [VERIFY]      */
#define FH_WS_SPI_HD      21   /* Bus-A DQ3 (QUADHD, QIO)        [VERIFY]      */
#define FH_WS_IRQ_OUT      9   /* head -> hub "data ready"       [VERIFY]      */

/* NOTE: GPIO1/3/5/6/7/8 are taken by the LCD+I2C (see the adapter). The Bus-A
 * placeholders above avoid those, but still VERIFY against the board's free
 * header pins + the ESP32-S3 strapping pins (0, 3, 45, 46) before wiring. */
