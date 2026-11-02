/*
 * fh_pico_pins.h — FHAdvanced-REMIX Pico SX1262 LoRa BLADE pin map
 * Board: Raspberry Pi Pico (RP2040) + Seeed Wio-SX1262 LoRa module.
 *
 * ##########################################################################
 * # HARDWARE SPLIT (Joshua 2026-10-25): the Seeed "XIAO ESP32S3 & Wio-SX1262 #
 * # Kit" is being SEPARATED. The Wio-SX1262 LoRa module is paired with a      #
 * # Pico here (this blade); the XIAO ESP32-S3 is retasked as the dedicated   #
 * # 2.4 GHz Wi-Fi sniffer (remix-FH-XIAO-S3-firmware). They no longer run    #
 * # together. So this Pico now drives the SX1262 directly over SPI + its     #
 * # control lines — including RF_SW1 and the TCXO-is-on-DIO3 detail that the #
 * # Wio module exposes.                                                      #
 * #                                                                          #
 * # This blade is a SPI SLAVE on the P4 hub's Bus B (binary fh_protocol).    #
 * # LoRa is the COMMS radio to the sensor stick (TX is CORE), NOT a capture  #
 * # sensor. Supports TX / listen-window / unsolicited RX per fh_ondemand.h.  #
 * # GPIO are GPn silkscreen numbers — VERIFY against the Pico + Wio module.  #
 * ##########################################################################
 */

#pragma once

/* ===========================================================================
 * SX1262 RADIO — Pico hardware SPI0 MASTER to the Wio-SX1262 module.
 * On the RP2040 set SPI0 pins on the `SPI` object BEFORE RadioLib begin().
 * The Wio-SX1262 exposes the standard SX1262 control set over its SPI header:
 * NSS (CS), SCK, MOSI, MISO, plus BUSY, DIO1 (IRQ), RESET. TCXO is powered from
 * DIO3 (module-internal, no host pin); RF_SW1 is a HOST control pin on the Wio
 * board (confirmed in gen1 research — NOT fully on-module), so the Pico owns it.
 * ======================================================================== */
#define FH_PICO_SPI_SCK     18     // GP18 — spi0 SCK   -> SX1262 SCK
#define FH_PICO_SPI_MOSI    19     // GP19 — spi0 TX    -> SX1262 MOSI
#define FH_PICO_SPI_MISO    16     // GP16 — spi0 RX    <- SX1262 MISO

#define FH_PICO_SX_NSS      17     // GP17 — SPI chip-select (NSS)
#define FH_PICO_SX_RST      12     // GP12 — reset
#define FH_PICO_SX_BUSY     11     // GP11 — busy (polled during ops)
#define FH_PICO_SX_DIO1     10     // GP10 — primary IRQ / event line
#define FH_PICO_SX_RFSW1     9     // GP9  — external RF-switch control (host pin)
/* DIO3 = TCXO supply is INTERNAL to the Wio-SX1262 — no host pin. RadioLib's
 * begin() sets tcxoVoltage (1.6 V default) which enables that control. */

/* ===========================================================================
 * HUB LINK — Pico hardware SPI1 SLAVE on the P4 hub's Bus B.
 * SEPARATE SPI block from the radio (spi0). Hub drives SCK + CS; data rides
 * MOSI (hub->blade) / MISO (blade->hub).
 * FIX (2026-10-31): the first draft mapped GP6/4/3/5 — that is an SPI0 mux
 * group, NOT SPI1, so SPISlave1 could not mux it and the link would never
 * clock. The radio's pins (GP18/19/16/17 + GP10/11/12/9) block the lower
 * SPI1 group (GP8..15), so this blade uses the FREE upper SPI1 group:
 * RX=GP24, TX=GP15, SCK=GP14, CSn=GP25. All other SPI1 pins are taken.
 * ======================================================================== */
#define FH_HUBLINK_SCK      14     // GP14 — spi1 SCK  (hub drives)
#define FH_HUBLINK_RX       24     // GP24 — spi1 RX = MOSI (hub -> blade)
#define FH_HUBLINK_TX       15     // GP15 — spi1 TX = MISO (blade -> hub)
#define FH_HUBLINK_CS       25     // GP25 — spi1 CSn  (hub selects)
#define FH_HUBLINK_IRQ       2     // GP2  — "RX/result ready" out to hub (optional)

/* ===========================================================================
 * PIN BUDGET (Pico non-W: 26 usable GPIO)
 *   SX1262 radio (SPI0) : 3 bus + NSS/RST/BUSY/DIO1/RFSW1 = 8  (GP18/19/16/17/12/11/10/9)
 *   Hub link (SPI1)     : 4 bus + 1 IRQ = 5                     (GP14/24/15/25/2)
 *   TOTAL = 13 of 26. Comfortable; the LoRa blade does one job well.
 *
 * NOTES
 *  - All Pico GPIO are 3.3V — matches the Wio-SX1262 (3.3V). No level shifting.
 *  - Keep the IPEX/U.FL ANTENNA attached before any TX — TX into an open can
 *    damage the PA. LoRa TX is authorized-use-only (comms to YOUR sensor stick).
 *  - RF_SW1 (GP9) toggles the Wio board's antenna switch between TX/RX paths;
 *    RadioLib + our code drive it. VERIFY the sense (which level = TX) on YOUR
 *    module rev at bench — gen1 left it LOW idle.
 *  - Watch the 3V3 rail under a LoRa TX current spike (+22 dBm PA); a bulk cap
 *    near the module helps.
 *  - VERIFY each GPn against the physical Pico + the Wio-SX1262 pin header
 *    before soldering (GP16/18/19 is one valid spi0 group; GP14/15/24/25 is a
 *    valid spi1 group — no clash with the radio pins).
 * ======================================================================== */
