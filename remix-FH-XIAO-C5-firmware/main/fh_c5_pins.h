/*
 * fh_c5_pins.h — FHAdvanced-REMIX XIAO-C5 pin map (SPI-slave to the P4 hub)
 * Node: Seeed XIAO ESP32-C5. Role: 5 GHz Wi-Fi sniffer (continuous) ONLY.
 *
 * ##########################################################################
 * # TRANSPORT CHANGED (REMIX 2026-10-25): the XIAO-C5 no longer talks to    #
 * # the hub over UART. It is now a SPI *SLAVE* on the hub's Bus A. The P4    #
 * # hub is the SPI master; this board presents its outbound frame queue when #
 * # the hub clocks it, and raises an IRQ line ("come clock me").            #
 * #                                                                          #
 * # GPIO numbers are the DOCUMENTED Seeed XIAO-C5 silkscreen->GPIO map       #
 * # (D8/D9/D10 = SCK/MISO/MOSI = GPIO8/9/10). VERIFY each against YOUR       #
 * # physical board. Capture LOGIC is unchanged from the gen1 firmware —      #
 * # ONLY the transport moved. Bus A is quad SPI SLAVE HD (spi_slave_hd,     #
 * # segment mode): the C5 HAS the HD slave peripheral (ESP-IDF             #
 * # SOC_SPI_SUPPORT_SLAVE_HD_VER2 = 1). DQ2/DQ3 = FH_C5_SPI_WP/_HD below.     #
 * # Bring up 1-bit HD first, then QIO (#9823) — see fh_c5_link.c L2.         #
 * ##########################################################################
 */
#pragma once

/* ===========================================================================
 * SPI-SLAVE LINK TO THE P4 HUB (fh_protocol frames over SPI).
 * Hub drives SCK + CS; data rides MOSI (hub->C5) / MISO (C5->hub).
 * XIAO-C5 silkscreen: D8=SCK=GPIO8, D9=MISO=GPIO9, D10=MOSI=GPIO10. VERIFY.
 * ======================================================================== */
#define FH_C5_SPI_SCK     8    // D8  / GPIO8  — SCK  (hub drives)   [VERIFY vs board]
#define FH_C5_SPI_MISO    9    // D9  / GPIO9  — MISO (C5 -> hub)    [VERIFY vs board]
#define FH_C5_SPI_MOSI   10    // D10 / GPIO10 — MOSI (hub -> C5)    [VERIFY vs board]
#define FH_C5_SPI_CS     12    // D7  / GPIO12 — CS   (hub selects)  [VERIFY vs board]

/* QUAD data lines DQ2/DQ3 (QUADWP/QUADHD) for 4-lane QIO Bus A (committed
 * 2026-11-01, ../../remix-BUS-A-QUAD-HD-DECISION.md). Used ONLY once the data
 * phase is widened to QIO; unused (harmless) in 1-bit-HD bring-up. PLACEHOLDER
 * free-ish XIAO-C5 GPIO — VERIFY against YOUR board + the hub's DQ2/DQ3
 * (GPIO5/20); pick pins not used by SCK/MISO/MOSI/CS/IRQ or strapping/USB. */
#define FH_C5_SPI_WP      1    // D0 / GPIO1 — DQ2 (QUADWP)  [VERIFY/choose]
#define FH_C5_SPI_HD      2    // D1 / GPIO2 — DQ3 (QUADHD)  [VERIFY/choose]

/* IRQ line OUT to the hub: driven HIGH when the TX queue has a frame ready;
 * idle LOW. The hub's matching input is GPIO30 on the P4 (per the hub pin
 * budget), rising-edge interrupt. Uses D6/GPIO11 (the old UART TX pin, free
 * now the UART link is gone). VERIFY GPIO11 is free + broken out on the board. */
#define FH_C5_IRQ_OUT    11    // D6  / GPIO11 — C5 -> HUB "data ready"  [VERIFY]

/* ===========================================================================
 * NOTES / BRING-UP OBLIGATIONS
 *  - 5 GHz capture: esp_wifi WIFI_BAND_MODE_5G_ONLY + set_channel. Start at
 *    20 MHz / WIFI_SECOND_CHAN_NONE, verify HE (11ax) RX, THEN widen (the
 *    IDF 6.0 HE/secondary-channel quirk status on 6.1 is unconfirmed).
 *  - The C5 is NEW SILICON — needs ESP-IDF 6.1 for the 5 GHz promiscuous API.
 *  - Console: USB-Serial-JTAG over the XIAO-C5 USB-C (keeps the SPI/IRQ pins).
 *  - MULTIDROP RULE: tri-state MISO unless CS is asserted (shared Bus A).
 *  - VERIFY strapping/boot/USB pins are not among those above on the board.
 * ======================================================================== */
