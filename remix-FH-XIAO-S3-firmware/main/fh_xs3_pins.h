/*
 * fh_xs3_pins.h — FHAdvanced-REMIX XIAO-S3 pin map (SPI-slave to the P4 hub)
 * Node: Seeed XIAO ESP32-S3. Role: 2.4 GHz Wi-Fi sniffer (continuous) ONLY.
 *
 * ##########################################################################
 * # TRANSPORT CHANGED (REMIX 2026-10-25): the XIAO-S3 no longer talks to    #
 * # the hub over UART. It is now a SPI *SLAVE* on the hub's Bus A (quad-SPI  #
 * # shared bus). The P4 hub is the SPI master; this board presents its       #
 * # outbound frame queue when the hub clocks it, and raises an IRQ line to   #
 * # tell the hub "I have data, come clock me."                              #
 * #                                                                          #
 * # GPIO numbers are the DOCUMENTED Seeed XIAO-S3 silkscreen->GPIO map       #
 * # (D8/D9/D10 = SCK/MISO/MOSI). VERIFY each against YOUR physical board at  #
 * # bring-up. The capture LOGIC is unchanged from the gen1 UART firmware —   #
 * # ONLY the transport moved.                                               #
 * ##########################################################################
 *
 * BUS A IS QUAD SPI SLAVE HD (committed — ../../remix-BUS-A-QUAD-HD-DECISION.md).
 * The ordinary full-duplex spi_slave driver is 1-bit only, so this board uses
 * the S3's SPI Slave HD peripheral (spi_slave_hd, segment mode) on all four data
 * lanes: DQ0 = MOSI, DQ1 = MISO, DQ2 = FH_XS3_SPI_WP, DQ3 = FH_XS3_SPI_HD
 * (GPIO1/GPIO2 placeholders below — VERIFY). The hub master drives it with
 * essl_spi_rddma / essl_spi_wrdma. #1 bench item: bring up 1-bit HD first, then
 * QIO and align dummy cycles (Espressif #9823) — fh_xs3_link.c L2.
 */
#pragma once

/* ===========================================================================
 * SPI-SLAVE LINK TO THE P4 HUB (fh_protocol frames over SPI).
 * The hub (master) drives SCK + CS; data rides MOSI (hub->S3) / MISO (S3->hub).
 * XIAO-S3 silkscreen: D8=SCK=GPIO7, D9=MISO=GPIO8, D10=MOSI=GPIO9. VERIFY.
 * ======================================================================== */
#define FH_XS3_SPI_SCK    7    // D8  / GPIO7  — SCK  (hub drives)   [VERIFY vs board]
#define FH_XS3_SPI_MISO   8    // D9  / GPIO8  — MISO (S3 -> hub)    [VERIFY vs board]
#define FH_XS3_SPI_MOSI   9    // D10 / GPIO9  — MOSI (hub -> S3)    [VERIFY vs board]
#define FH_XS3_SPI_CS    44    // D7  / GPIO44 — CS   (hub selects)  [SEE CS/IRQ NOTE]

/* QUAD data lines DQ2/DQ3 (QUADWP/QUADHD) for 4-lane QIO Bus A (committed
 * 2026-11-01, ../../remix-BUS-A-QUAD-HD-DECISION.md). Used ONLY once the data
 * phase is widened to QIO; unused (but harmless) in 1-bit-HD bring-up. These
 * are PLACEHOLDER free-ish XIAO-S3 GPIO — VERIFY against YOUR board and the
 * hub's DQ2/DQ3 (GPIO5/20) wiring; pick two pins not used by SCK/MISO/MOSI/CS/
 * IRQ or strapping/USB. */
#define FH_XS3_SPI_WP     1    // D0 / GPIO1 — DQ2 (QUADWP)  [VERIFY/choose]
#define FH_XS3_SPI_HD     2    // D1 / GPIO2 — DQ3 (QUADHD)  [VERIFY/choose]

/* IRQ line OUT to the hub: this board drives it HIGH when its TX queue has a
 * frame ready ("come clock me"). Idle LOW. The hub configures its matching
 * input (GPIO29 on the P4, per the hub pin budget) as a rising-edge interrupt. */
#define FH_XS3_IRQ_OUT   43    // D6  / GPIO43 — S3 -> HUB "data ready"  [SEE NOTE]

/* ★ CS/IRQ PIN SENSITIVITY (carried gen1 ABYSS-S3-1 — do NOT ignore):
 * D6/D7 = GPIO43/44 are the ESP32-S3's UART0 console pins. The gen1 firmware
 * deliberately avoided them for a second UART because UART0 owns them at
 * bootloader time. Here they carry SPI CS + a GPIO IRQ, NOT a UART, and the
 * console is on USB-CDC — so they are USABLE, but they are NOT risk-free:
 * UART0 may toggle them during boot/reset, which could glitch CS/IRQ before the
 * app configures them. VERIFY on the bench that boot-time UART0 activity does
 * not spuriously select this slave or fire a false IRQ. If it does, move CS/IRQ
 * to two of the known-free SPI-unused pins and update the hub side to match.
 * This is called out, not buried, precisely because it bit the project before. */

/* ===========================================================================
 * NOTES / BRING-UP OBLIGATIONS
 *  - 2.4 GHz capture: esp_wifi promiscuous; dedup seen-table on the board;
 *    emit CAPTURE / SUMMARY / HEARTBEAT (fh_protocol) — UNCHANGED from gen1.
 *  - Console: USB-CDC over the XIAO-S3 USB-C (so CS/IRQ on GPIO43/44 are not
 *    fighting a wired UART0 console — but see the CS/IRQ sensitivity note above).
 *  - MULTIDROP RULE: Bus A is shared with the other peers. This slave MUST
 *    tri-state MISO unless its CS is asserted, or it will fight the bus. The
 *    ESP-IDF SPI slave driver releases MISO when CS is high; confirm at bench.
 *  - VERIFY strapping/boot/USB pins are not among those above on the board.
 * ======================================================================== */
