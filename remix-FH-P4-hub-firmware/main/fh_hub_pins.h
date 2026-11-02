/*
 * fh_hub_pins.h — FHAdvanced-REMIX HUB pin map
 * Node: Waveshare ESP32-P4-WIFI6-DB  (the NEW hub / bus master).
 *
 * ##########################################################################
 * # VERIFIED against remix-FH-Gen2-PIN-BUDGET-BASELINE.md (2026-10-25):     #
 * #  - every GPIO here is header-exposed on the 2x20 headers,               #
 * #  - none collide with onboard C6 SDIO / I2S audio / SD card / USB-OTG /  #
 * #    UART0 console / strapping pins,                                      #
 * #  - quad-SPI on GPIO2-5+20 is legal via the P4 GPIO matrix (~26-40 MHz,  #
 * #    ample for distilled-event traffic — see the baseline doc).           #
 * # Pin LOGIC proven by reading only; NOT bench-confirmed on silicon.       #
 * ##########################################################################
 *
 * THE HUB'S THREE JOBS (owner decision 2026-10-25):
 *   1. COORDINATE the peripheral devices over two SPI buses.
 *   2. WRITE the unified log to the microSD card (single writer).
 *   3. EMIT display data to the Waveshare-S3 HEAD (a Bus-A peer that renders).
 * Plus: hub-local BLE sniff on the P4's own radio (off-bus). No LCD on the P4.
 *
 * TWO SPI BUSES (the framework / the pipeline):
 *   BUS A  quad-SPI master, 3 ESP32 PEER slaves, IRQ-driven:
 *            - Waveshare-S3 head (display sink)   -> peer slot
 *            - XIAO-S3  (2.4 GHz Wi-Fi sniffer)   -> peer slot
 *            - XIAO-C5  (5 GHz Wi-Fi sniffer)     -> peer slot
 *          A peer raises its IRQ ("I have a frame"); the hub clocks it in.
 *   BUS B  conventional SPI master, up to 6 BLADE slaves, CS per blade:
 *            - Pico+radio blades (CC1101 / nRF24 / PN532 / SX1262 / GPS ...)
 *          On-demand, one at a time (BUSY contract). GPS is one such blade.
 *
 * The hub hosts NO wired radios itself. Capability is MODULAR: a peer or blade
 * plugs into the framework by speaking fh_protocol over its bus — the hub does
 * not change when capability is added.
 */

#pragma once

#include <stdint.h>

/* ===========================================================================
 * BUS A — shared QUAD-SPI to the 3 ESP32 peers (8 signal + 3 IRQ = 11 GPIO)
 * Shared CLK + DQ0..3; one CS per peer; one IRQ per peer. Time-sliced: exactly
 * one peer is selected (CS low) at a time. Runs through the P4 GPIO matrix.
 * ======================================================================== */
#define FH_BUSA_CLK          2    /* QSPI_CLK   — shared clock (output)         */
#define FH_BUSA_DQ0          3    /* QSPI_DQ0   — shared data 0 (bidir)         */
#define FH_BUSA_DQ1          4    /* QSPI_DQ1   — shared data 1 (bidir)         */
#define FH_BUSA_DQ2          5    /* QSPI_DQ2   — shared data 2 (bidir)         */
#define FH_BUSA_DQ3          20   /* QSPI_DQ3   — shared data 3 (bidir)         */

#define FH_BUSA_CS_HEAD      21   /* peer #1 CS — Waveshare-S3 head (display)   */
#define FH_BUSA_CS_XS3       22   /* peer #2 CS — XIAO-S3 (2.4 GHz Wi-Fi)       */
#define FH_BUSA_CS_XC5       23   /* peer #3 CS — XIAO-C5 (5 GHz Wi-Fi)         */

#define FH_BUSA_IRQ_HEAD     28   /* peer #1 IRQ in — head wants a render ack / */
                                  /*                  touch/UI event upstream   */
#define FH_BUSA_IRQ_XS3      29   /* peer #2 IRQ in — XS3 has capture frame(s)  */
#define FH_BUSA_IRQ_XC5      30   /* peer #3 IRQ in — XC5 has capture frame(s)  */

/* Logical peer indices (array order; NOT a wire value). The wire src_id for a
 * capture sender is separate and FROZEN in fh_frame.h (c5=1, xs3=2, pico=3);
 * the head carries FH_SRC_HEAD=4 for LIVENESS tracking only (no capture). */
typedef enum {
    FH_PEER_HEAD = 0,   /* Waveshare-S3 display head (wire src_id FH_SRC_HEAD=4;
                         * liveness-only: PING/PONG, no capture)              */
    FH_PEER_XS3,        /* XIAO-S3 2.4 GHz  (wire src_id FH_SRC_XS3_WIFI24=2)  */
    FH_PEER_XC5,        /* XIAO-C5 5 GHz    (wire src_id FH_SRC_C5_WIFI5=1)    */
    FH_PEER_COUNT
} fh_peer_t;

/* ===========================================================================
 * BUS B — shared CONVENTIONAL SPI to the blades (3 signal + 6 CS = 9 GPIO)
 * Shared SCLK/MOSI/MISO; one CS per blade. On-demand, one blade at a time.
 * ======================================================================== */
#define FH_BUSB_SCLK         31   /* shared clock (output)                      */
#define FH_BUSB_MOSI         32   /* shared data hub -> blade (output)          */
#define FH_BUSB_MISO         33   /* shared data blade -> hub (input)           */

#define FH_BUSB_CS1          46   /* blade #1 CS                                */
#define FH_BUSB_CS2          47   /* blade #2 CS                                */
#define FH_BUSB_CS3          48   /* blade #3 CS                                */
#define FH_BUSB_CS4          49   /* blade #4 CS                                */
#define FH_BUSB_CS5          50   /* blade #5 CS                                */
#define FH_BUSB_CS6          51   /* blade #6 CS                                */

#define FH_BLADE_COUNT       6    /* max blade slots on Bus B                   */

/* Blade radio <-> CS assignment is NOT pinned here (wiring-time decision; the
 * hub is radio-agnostic). Most blades are FIRED (request/response on operator
 * action, BUSY one-at-a-time). The GPS blade is the one exception: it is
 * PERIODIC, so the hub POLLS it on a schedule (cadence from cfg.gps_rate,
 * 1 Hz / 5 Hz) rather than waiting for a fire. The GPS blade firmware (Pico +
 * NEO-6M, rewritten for SPI later this session) answers each poll with a
 * FROZEN FH_TYPE_GPS_FIX (0x02) frame. A 5 Hz GPS poll is ~16 bytes / 200 ms
 * = well under 0.1% bus occupancy, so it does not disturb fired radio ops that
 * share Bus B. Which CS the GPS blade sits on is a wiring-time choice; the hub
 * learns it from config below (defaults to slot 0 for pass 1). */
#define FH_BLADE_SLOT_GPS    0u   /* default GPS blade CS slot (0..5); wiring   */
                                  /* may move it — this is the pass-1 default   */

/* PER-RADIO BLADE SLOTS (REMIX: one radio per blade — the gen2 topology).
 * Each blade is a (pico + radio). The gen1 design had ONE Pico carrying all
 * four radios; REMIX split them into separate single-radio blades. The hub
 * addresses EACH radio by its OWN CS slot. BASE BLADES FIRST: GPS and LoRa are
 * on essentially every device, so they take slots 0 and 1; the fired recon
 * radios follow. Any can move at wiring time (map updated here + the CS wire).*/
#define FH_BLADE_SLOT_SX1262 1u   /* LoRa comms blade (base — almost always present) */
#define FH_BLADE_SLOT_CC1101 2u   /* sub-GHz OOK blade                          */
#define FH_BLADE_SLOT_NRF24  3u   /* 2.4 GHz nRF24 blade                        */
#define FH_BLADE_SLOT_PN532  4u   /* NFC blade — PARKED, slot reserved (see ../../remix-PARKED-CARD-READER-RADIOS.md) */
/* slot 5 spare. GPS=slot 0 (polled), LoRa=slot 1 — the base pair. */

/* Back-compat alias: some callers (the host bridge's generic relay) still refer
 * to a single "Pico on-demand" slot. It aliases the CC1101 blade for now; a
 * proper requester routes per-radio via fh_link_slot_for_radio() (see link). */
#define FH_BLADE_SLOT_PICO   FH_BLADE_SLOT_CC1101

/* ===========================================================================
 * microSD — the single log sink.
 * On the ESP32-P4-WIFI6-DB the TF slot is a DEDICATED SDMMC 4-bit interface on
 * GPIO39..45 (NOT on the header, NOT on either SPI bus). The logger uses the
 * SDMMC host; these pins are fixed by the board and are documented here for
 * reference only — the SDMMC driver owns them, they are never in a bus budget.
 *   GPIO39..42 SD_D0..D3,  GPIO43 SD_CLK,  GPIO44 SD_CMD,  GPIO45 SD_VDD_EN.
 * ======================================================================== */
#define FH_SD_MOUNT_POINT    "/sdcard"

/* ===========================================================================
 * SPARE header GPIO (from the verified baseline; not used in pass 1):
 *   GPIO7  — I2C SDA (board shared I2C; the head owns LCD+touch, so FREE here)
 *   GPIO8  — I2C SCL (same)
 *   GPIO24 — USB-OTG D- (free only if USB-OTG unused)
 *   GPIO25 — USB-OTG D+ (free only if USB-OTG unused)
 *   GPIO26, GPIO27, GPIO52 — unconditionally free
 * Reserve GPIO7/8 as the hub's general I2C if ever needed (RTC/fuel-gauge).
 * ======================================================================== */

/* ===========================================================================
 * CONSOLE + BLE (off-bus, no header pins)
 *   Console: UART0 console (onboard CH343 USB-UART on the USB-C, 921600) (idf.py flash monitor). This is also
 *     the Medusa host-bridge transport (see fh_hub_bridge).
 *   BLE: the P4's onboard ESP32-C6 co-processor (SDIO GPIO14..19, dedicated,
 *     zero header cost) gives the hub Wi-Fi6 + BLE. BLE SNIFF is hub-local —
 *     pass 1 leaves it a stub hook; pass 2 turns it on. See fh_hub_ble.
 * ======================================================================== */
