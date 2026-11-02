/*
 * nrf_shim.h — minimal nRF24L01(+) driver for the native Pico SDK.
 *
 * Purpose-written (permissive, FH-owned — NOT a third-party lib) because no
 * clean permissively-licensed pico-sdk nRF24 driver exposes what this blade's
 * verbs need: the RPD/carrier read for SCAN and raw payload capture RX for
 * SNIFF. (AndyRids/pico-nrf24 is GPL-2.0 → rejected to keep the project
 * permissive; TMRh20 RF24's non-Arduino path is Linux spidev, not pico-sdk.)
 *
 * Scope: exactly the operations the fh_ondemand nRF24 verbs use —
 *   present / init / set-channel / RX-mode / idle / RPD read /
 *   static-width payload RX / payload TX.
 * Register map + command set are from the Nordic nRF24L01+ datasheet (public
 * facts). The nRF24 drives spi1 (master, MODE 0); CSN is SOFTWARE-driven GPIO
 * (not the SPI hardware CS), CE is the RX/TX enable.
 *
 * Plain C; called from main.c directly (no extern "C" needed — this TU is C).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Bring up spi1 + CSN/CE and probe the chip. Returns true if the nRF24 answers
 * (a CONFIG-register read/write round-trips). Leaves the radio idle (powered
 * up, RX primed, CE low). */
bool nrf_init(uint8_t channel);

/* Re-probe: confirm the chip still round-trips a register (used for PING). */
bool nrf_present(void);

/* Set the RF channel (0..125 -> 2400+ch MHz). */
void nrf_set_channel(uint8_t channel);

/* Enter RX mode on the current channel (CE high, PRIM_RX=1). For SNIFF pass
 * open_capture=true: CRC + auto-ack OFF, STATIC 32-byte payload width (dynamic
 * payloads are NOT used — DPL requires auto-ack, which defeats capture). NOTE
 * this is NOT true promiscuity — it still only receives on the default pipe-0
 * address (0xE7E7E7E7E7), which is what this blade's own TX uses. See
 * R-PROMISC. Pass false for a normal framed RX (CRC on). */
void nrf_rx_mode(bool open_capture);

/* Drop to standby (CE low). */
void nrf_idle(void);

/* Received-power detector: the nRF24's 1-bit RPD/CD (channel busy). Must be in
 * RX mode and have dwelt long enough. Returns true if a carrier was detected.
 * (There is NO real dBm RSSI on the nRF24 — this is the only signal.) */
bool nrf_rpd(void);

/* If a payload is waiting, read it into buf. Capture mode uses a STATIC 32-byte
 * payload width (DPL off), so this returns min(32, cap) bytes when a packet is
 * ready, else 0. There is NO per-packet length on the wire in this mode — the
 * trailing bytes beyond the real payload are FIFO content. RX mode only. */
int nrf_rx_payload(uint8_t *buf, int cap);

/* Transmit a 1..32-byte payload on the current channel to the default address.
 * Returns true if the chip reported the packet sent (TX_DS). Always transmits
 * with an EXPLICIT 2-byte CRC (does not inherit a prior SNIFF's CRC-off config).
 * Does NOT restore the prior RX state: it leaves the radio in TX-standby
 * (PRIM_RX=0, CE low). Callers must re-enter RX via nrf_rx_mode() (main.c calls
 * nrf_idle() after TX, and every RX path re-runs nrf_rx_mode()). autoAck off. */
bool nrf_tx_payload(const uint8_t *buf, int len);
