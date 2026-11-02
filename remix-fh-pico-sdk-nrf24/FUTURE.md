# FUTURE.md — remix-fh-pico-sdk-nrf24 (what's next)

Forward notes for the nRF24 blade. Pair with `CONTINUITY.md` and
`../remix-PICO-SDK-PORT-PLAN.md`.

## Place in the plan
The LAST of the three committed radio ports. All three (cc1101, sx1262, nrf24)
now exist as pico-sdk first drafts. The remaining program-level item is retiring
the arduino blades once the SDK ports are bench-proven (owner decision pending).

## Bench-gated (must prove on hardware)
1. **First build** — all C, no external deps; confirm it builds + flashes.
2. **Hub link carries** over the copied spi0 pump: PING→PONG, PING_REQ→RESP.
3. **Driver bring-up (R-DRIVER).** `nrf_init` round-trips RF_CH; PING_RESP shows
   the nRF24 bit. This is from-scratch code — the first suspect for any radio
   failure. Check register addresses/bits and CE/standby timing against the
   datasheet if it misbehaves.
4. **SCAN.** RPD sweep finds a known transmitter's channel.
5. **SNIFF.** Capture-to-dump streams payloads from a known transmitter on the
   dwell channel; Medusa reassembles the chunks.
6. **ext TX (0x3C)** to your own receiver.

## Smaller items / follow-ups
- **Full promiscuity + true lengths (R-PROMISC).** The current sniff captures
  packets on the default pipe-0 address with CRC/aa off and a STATIC 32-byte
  payload width (so every record is 32 bytes — no per-packet length). If you
  want arbitrary 2.4 GHz traffic and/or real lengths, extend `nrf_shim` with the
  classic 2-byte-address hack (SETUP_AW illegal value + a wildcard RX_ADDR).
  Keep it in the driver.
- **IRQ pin.** Not wired; the framework polls STATUS. If sniff throughput needs
  it, wire the nRF24 IRQ to a spare Pico GPIO and add an ISR — but mind that
  core1 owns the radio, so keep any IRQ on core1.
- **Driver hardening.** As the only from-scratch radio driver, `nrf_shim.c`
  would benefit from a bench-validated pass (payload-width edge cases, FIFO
  flush paths, the TX_DS/MAX_RT wait loop timing).
- **Release artifact.** Add a `releases/` prebuilt `.uf2` once bench-proven.

## Explicitly rejected (do not resurrect)
- Vendoring `AndyRids/pico-nrf24` or any GPL/LGPL nRF24 driver (keeps the
  project permissive). `nrf_shim.c` stays FH-owned.
- Redesigning the hub pump (it is the proven GPS pump).
- Staging from core1 (core0 is the single writer).
- Inventing a dBm RSSI (the nRF24 has none — RPD channel presence only).
- Unifying the two SPI modes (hub MODE 1 / radio MODE 0 both correct).
