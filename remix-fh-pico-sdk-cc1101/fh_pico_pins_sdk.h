/*
 * fh_pico_pins_sdk.h — FHAdvanced-REMIX Pico CC1101 BLADE pin map
 *          (native Raspberry Pi Pico C/C++ SDK).
 *
 * TWO SPI BLOCKS (decision 2026-11-01, see ../remix-PICO-SDK-PORT-PLAN.md):
 *   HUB LINK  = spi0 SLAVE  (hub is master; MODE 1). Copied verbatim from the
 *               bench-proven GPS blade pump — same pins, left on spi0 because
 *               that is the proven configuration.
 *   RADIO     = spi1 MASTER (Pico drives the CC1101 chip; CC1101 is MODE 0).
 *               The vetted driver's `#define SPI_BUS` is set to spi1 to match.
 *
 * GPIO are GPn silkscreen numbers — VERIFY against the Pico + CC1101 module.
 * CC1101 VCC is 3.3V ONLY (Pico 3V3 pin 36, never 5V/VBUS).
 */
#pragma once

/* ---- HUB LINK: spi0 slave (hub Bus B). Same as the GPS blade (proven). ---- */
#define FH_PICO_SPI_SCK     18   /* spi0 SCK  (hub drives)                      */
#define FH_PICO_SPI_MOSI    16   /* hub MOSI -> blade input: spi0 RX on slave   */
#define FH_PICO_SPI_MISO    19   /* blade output -> hub MISO: spi0 TX on slave  */
#define FH_PICO_SPI_CS      21   /* GP21 — one of the four valid spi0 CSn pins  */

#define FH_PICO_IRQ_OUT      6   /* optional "result ready" out to hub (unused) */

/* ---- RADIO: spi1 master to the CC1101 chip. ------------------------------ *
 * Valid spi1 pin group on the RP2040: SCK=GP10, TX(MOSI)=GP11, RX(MISO)=GP12,
 * CSn=GP13. GDO0/GDO2 are plain GPIO the driver + capture path drive.         */
#define FH_CC_SPI_SCK       10   /* spi1 SCK  -> CC1101 SCK                      */
#define FH_CC_SPI_MOSI      11   /* spi1 TX   -> CC1101 MOSI (SI)                */
#define FH_CC_SPI_MISO      12   /* spi1 RX   <- CC1101 MISO (SO)               */
#define FH_CC_SPI_CSN       13   /* CC1101 CSN (chip select)                    */
#define FH_CC_GDO0          14   /* RX data / capture-IRQ + internal TX sync    */
#define FH_CC_GDO2          15   /* reserved (unused in v0)                     */

#define FH_PICO_LED         25   /* on-board LED                                */

#define FH_PICO_DBG_TX       0   /* optional uart0 debug, OFF unless FH_DEBUG_UART */
#define FH_PICO_DBG_RX       1

/* Transport sizing: hub clocks exactly FH_MAX_FRAME (261) bytes per
 * transaction, TWO transactions per poll (request, then response read). */
#define FH_SPI_XFER_BYTES   FH_MAX_FRAME
