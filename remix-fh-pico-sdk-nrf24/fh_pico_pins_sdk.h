/*
 * fh_pico_pins_sdk.h — FHAdvanced-REMIX Pico nRF24 BLADE pin map
 *          (native Raspberry Pi Pico C/C++ SDK).
 *
 * TWO SPI BLOCKS (decision 2026-11-01, see ../remix-PICO-SDK-PORT-PLAN.md):
 *   HUB LINK  = spi0 SLAVE  (hub is master; MODE 1). Copied verbatim from the
 *               bench-proven GPS blade pump — same pins, left on spi0.
 *   RADIO     = spi1 MASTER (Pico drives the nRF24L01+). nRF24 is a MODE-0
 *               device; the hand-written nrf_shim.c sets CPOL0/CPHA0 on spi1.
 *
 * NOTE THE SPLIT DIFFERS FROM THE ARDUINO nrf24 BLADE (radio on SPI0, hub on
 * SPI1). The SDK port inverts it so the hub pump stays byte-identical to the
 * proven GPS blade on spi0. (Same inversion as the cc1101/sx1262 SDK ports.)
 *
 * nRF24 uses CE + CSN as its two control pins (NOT a CC1101-style GDO). VCC is
 * 3.3V ONLY (Pico 3V3 pin 36). A 10uF cap across the module VCC/GND is strongly
 * recommended (esp. +PA+LNA) to stop brown-out resets. GPIO are GPn silkscreen
 * numbers — VERIFY against the Pico + module.
 */
#pragma once

/* ---- HUB LINK: spi0 slave (hub Bus B). Same as the GPS blade (proven). ---- */
#define FH_PICO_SPI_SCK     18   /* spi0 SCK  (hub drives)                      */
#define FH_PICO_SPI_MOSI    16   /* hub MOSI -> blade input: spi0 RX on slave   */
#define FH_PICO_SPI_MISO    19   /* blade output -> hub MISO: spi0 TX on slave  */
#define FH_PICO_SPI_CS      21   /* GP21 — one of the four valid spi0 CSn pins  */

#define FH_PICO_IRQ_OUT      2   /* optional "RX/result ready" out to hub       */

/* ---- RADIO: spi1 master to the nRF24L01+. -------------------------------- *
 * Valid spi1 pin group on the RP2040: SCK=GP10, TX(MOSI)=GP11, RX(MISO)=GP12.
 * CSN + CE are plain GPIO the nrf_shim drives (CSN = SPI chip-select done in
 * software; CE = RX/TX enable). nRF24 has NO IRQ pin wired (framework polls).  */
#define FH_NRF_SPI_SCK      10   /* spi1 SCK  -> nRF24 SCK                       */
#define FH_NRF_SPI_MOSI     11   /* spi1 TX   -> nRF24 MOSI                      */
#define FH_NRF_SPI_MISO     12   /* spi1 RX   <- nRF24 MISO                      */
#define FH_NRF_CSN          13   /* chip-select (software-driven, active low)   */
#define FH_NRF_CE            7   /* chip-enable (RX/TX toggle)                  */
/* nRF24 IRQ pin: not wired (the framework polls STATUS). Leave unconnected.    */

#define FH_PICO_LED         25   /* on-board LED                                */

#define FH_PICO_DBG_TX       0   /* optional uart0 debug, OFF unless FH_DEBUG_UART */
#define FH_PICO_DBG_RX       1

/* Transport sizing: hub clocks exactly FH_MAX_FRAME (261) bytes per
 * transaction, TWO transactions per poll (request, then response read). */
#define FH_SPI_XFER_BYTES   FH_MAX_FRAME

/* PIN BUDGET (Pico non-W, 26 usable GPIO)
 *   Hub link (spi0): SCK18/MOSI16/MISO19/CS21 + IRQ2        = 5
 *   Radio   (spi1) : SCK10/MOSI11/MISO12 + CSN13/CE7        = 5
 *   LED 25, dbg 0/1 (optional)
 * VERIFY: GP10/11/12 is a valid spi1 group; GP16/18/19 a valid spi0 group; no
 * pin shared between the two blocks. Confirm on YOUR Pico silkscreen. */
