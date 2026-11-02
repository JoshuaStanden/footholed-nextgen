/*
 * fh_pico_pins_sdk.h — FHAdvanced-REMIX Pico GPS BLADE pin map
 *          (native Raspberry Pi Pico C/C++ SDK port of fh_pico_pins.h)
 *
 * SAME MAPPING as the Arduino version (fh_pico_pins.h in the blade sketch dir):
 *   SPI slave (spi0, hub Bus B, hub is MASTER):
 *     SCK  = GP18   (hub drives)
 *     MOSI = GP16   (hub -> blade; on the RP2040 this is spi0 RX in slave mode)
 *     MISO = GP19   (blade -> hub; spi0 TX in slave mode)
 *     CS   = GP21   (hub selects; RP2040 spi0 CSn pins are ONLY
 *                    GP1/GP5/GP17/GP21 — GP21 = physical pin 27)
 *   GPS NEO-6M on uart1:
 *     TX   = GP4    (blade -> GPS RX, UBX 5 Hz config)
 *     RX   = GP5    (GPS TX -> blade, NMEA)
 *   LED  = GP25 (on-board)
 *   IRQ OUT (optional "fresh fix" to hub, unused by current hub) = GP6
 *   DEBUG (optional, off by default): uart0, GP0 TX / GP1 RX, 115200 —
 *     enabled only if FH_DEBUG_UART is defined to 1 at build time.
 *
 * GPIO are GPn numbers — VERIFY against the Pico silkscreen at wiring.
 */

#pragma once

#define FH_PICO_SPI_SCK     18
#define FH_PICO_SPI_MOSI    16   /* hub MOSI -> blade input: spi0 RX on slave */
#define FH_PICO_SPI_MISO    19   /* blade output -> hub MISO: spi0 TX on slave */
#define FH_PICO_SPI_CS      21   /* GP21 — one of the four valid spi0 CSn pins */

#define FH_PICO_IRQ_OUT      6

#define FH_PICO_GPS_TX       4
#define FH_PICO_GPS_RX       5

#define FH_PICO_LED          25

#define FH_PICO_DBG_TX       0
#define FH_PICO_DBG_RX       1

/* Transport sizing: the hub clocks exactly FH_MAX_FRAME (261) bytes per
 * transaction, TWO transactions per poll (request, then response read). */
#define FH_SPI_XFER_BYTES    FH_MAX_FRAME
