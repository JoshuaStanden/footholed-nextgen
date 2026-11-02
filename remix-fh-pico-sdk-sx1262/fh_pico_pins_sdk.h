/*
 * fh_pico_pins_sdk.h — FHAdvanced-REMIX Pico SX1262 LoRa BLADE pin map
 *          (native Raspberry Pi Pico C/C++ SDK).
 *
 * TWO SPI BLOCKS (decision 2026-11-01, see ../remix-PICO-SDK-PORT-PLAN.md):
 *   HUB LINK  = spi0 SLAVE  (hub is master; MODE 1). Copied verbatim from the
 *               bench-proven GPS blade pump — same pins, left on spi0 because
 *               that is the proven configuration.
 *   RADIO     = spi1 MASTER (Pico drives the Wio-SX1262). RadioLib's PicoHal is
 *               constructed with `spi1`; the SX1262 is a MODE-0 device and the
 *               HAL's spiBegin() uses CPOL0/CPHA0 accordingly.
 *
 * NOTE THE SPLIT DIFFERS FROM THE ARDUINO sx1262 BLADE (radio on SPI0, hub on
 * SPI1). The SDK port inverts it so the hub pump stays byte-identical to the
 * proven GPS blade on spi0. (Same inversion as the cc1101 SDK port.)
 *
 * GPIO are GPn silkscreen numbers — VERIFY against the Pico + Wio-SX1262.
 * All Pico GPIO + the Wio module are 3.3V — no level shifting. Keep the
 * ANTENNA attached before any TX (+22 dBm PA). LoRa TX is authorized-use-only
 * (comms to YOUR sensor stick), never auto.
 */
#pragma once

/* ---- HUB LINK: spi0 slave (hub Bus B). Same as the GPS blade (proven). ---- */
#define FH_PICO_SPI_SCK     18   /* spi0 SCK  (hub drives)                      */
#define FH_PICO_SPI_MOSI    16   /* hub MOSI -> blade input: spi0 RX on slave   */
#define FH_PICO_SPI_MISO    19   /* blade output -> hub MISO: spi0 TX on slave  */
#define FH_PICO_SPI_CS      21   /* GP21 — one of the four valid spi0 CSn pins  */

#define FH_PICO_IRQ_OUT      2   /* optional "RX/result ready" out to hub       */

/* ---- RADIO: spi1 master to the Wio-SX1262. -------------------------------- *
 * Valid spi1 pin group on the RP2040: SCK=GP10, TX(MOSI)=GP11, RX(MISO)=GP12.
 * NSS/RST/BUSY/DIO1 are plain GPIO RadioLib's Module() + PicoHal drive. RF-SW
 * is the module's antenna-switch pad (silkscreen "RF-SW"); the Pico drives it.
 * Pads confirmed from the Wio-SX1262 board silkscreen — see WIRING.md.         */
#define FH_SX_SPI_SCK       10   /* spi1 SCK  -> SX1262 SCK                      */
#define FH_SX_SPI_MOSI      11   /* spi1 TX   -> SX1262 MOSI                     */
#define FH_SX_SPI_MISO      12   /* spi1 RX   <- SX1262 MISO                     */
#define FH_SX_NSS           13   /* SPI chip-select (NSS)                        */
#define FH_SX_RST            8   /* reset                                       */
#define FH_SX_BUSY           9   /* busy (RadioLib polls during ops)            */
#define FH_SX_DIO1           7   /* primary IRQ / event line (RadioLib IRQ)     */
#define FH_SX_RFSW1          6   /* RF/antenna switch — module silkscreen "RF-SW" */
/* DIO3 = TCXO supply is INTERNAL to the Wio-SX1262 — no host pin. RadioLib's
 * begin() tcxoVoltage (1.6 V) enables it. */

#define FH_PICO_LED         25   /* on-board LED                                */

#define FH_PICO_DBG_TX       0   /* optional uart0 debug, OFF unless FH_DEBUG_UART */
#define FH_PICO_DBG_RX       1

/* Transport sizing: hub clocks exactly FH_MAX_FRAME (261) bytes per
 * transaction, TWO transactions per poll (request, then response read). */
#define FH_SPI_XFER_BYTES   FH_MAX_FRAME

/* LoRa payload bound. The on-demand LoRa structs (fh_od_lora_tx_req_t,
 * fh_od_lora_rx_ind_t) carry data[200], so our staging buffers must bound to
 * 200 — NOT fh_frame.h's FH_LORA_DATA_MAX, which is the larger frame-relay
 * payload (FH_MAX_PAYLOAD-2, ~253) and would overrun the 200-byte on-demand
 * structs. Named distinctly to avoid colliding with that macro. */
#define FH_OD_LORA_PAYLOAD_MAX   200

/* PIN BUDGET (Pico non-W, 26 usable GPIO)
 *   Hub link (spi0): SCK18/MOSI16/MISO19/CS21 + IRQ2            = 5
 *   Radio   (spi1) : SCK10/MOSI11/MISO12 + NSS13/RST8/BUSY9/DIO1-7/RFSW1-6 = 8
 *   LED 25, dbg 0/1 (optional)                                  = 1 (+2)
 *   TOTAL = 14 of 26. Comfortable.
 * VERIFY: GP10/11/12 is a valid spi1 group; GP16/18/19 a valid spi0 group;
 * no pin shared between the two blocks. Confirm on YOUR Pico silkscreen. */
