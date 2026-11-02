# AGENTS.md — remix-FH-P4-hub-firmware (for AI / LLM agents)

Read this BEFORE editing the hub. The repo-root `../AGENTS.md` governs the whole
rig (RP2040 MODE-1 rule, no-compile-here, flash procedure) — read it first. This
file carries the hub-specific lore. The hub is **bench-proven to build/boot/BLE**
(ESP-IDF 6.1, P4-WIFI6-DB); read the README "Verification status" before trusting
a pin or a call. Five-doc set: WIRING / README / AGENTS (this) / CONTINUITY /
FUTURE.

## Non-negotiables (this hub)
1. **Two SPI buses, two hosts, swapped for a reason.** Bus A (quad, 3 ESP32
   peers) = **SPI3**; Bus B (conventional, 6 blades) = **SPI2**. The swap is
   because SPI2 has 6 hardware CS lines and SPI3 has only 3 — Bus B needs 6. Do
   NOT swap them back (the 4th `spi_bus_add_device` on SPI3 failed ESP_ERR_NO_MEM).
2. **SPI mode per bus:** Bus B `dc.mode = 1` (RP2040 blades need MODE 1 — the
   one-byte-per-CS rule); Bus A `dc.mode = 0` (ESP32 peers stay MODE 0). Do NOT
   flip either. The MODE-1 rule is for RP2040 slaves only; Bus A peers are ESP32.
3. **One dedicated mutex per bus, never nested.** Every transaction on a bus
   takes its bus mutex, held only across `spi_device_transmit`, released before
   the decoder push. Separate buses = separate mutexes. The decoder/route_frame
   sinks run OUTSIDE the bus mutex so a slow sink never stalls the bus. This is
   the carried gen1 "new sink → new mutex" rule — the hub-bridge race bit the
   project 3× in gen1; do not reintroduce a shared or nested lock.
4. **Two-phase blade xfer.** A SPI-slave blade cannot answer within the frame
   that delivered the request — `fh_link_blade_xfer` delivers in phase 1, waits a
   gap, reads the staged reply in phase 2. The on-demand requester polls with
   idle frames until the matching seq; it NEVER re-sends the REQ (that re-runs a
   completed blade op). Verified against the blade sources, not assumed.
5. **Liveness by frame + src_id.** `fh_health_on_frame(src_id)` marks any node
   alive from any validated frame (Pico blades don't heartbeat). A blade slot
   with `src_id = 0` is invisible to the health monitor even while its frames
   flow — the GPS slot and the radio slot both carry `FH_SRC_PICO`.
6. **The frozen protocol is byte-identical.** `components/fh_protocol/` is copied
   verbatim across every firmware. Never hand-edit a struct here without updating
   every copy and the `_Static_assert` guards; Medusa parses these headers and
   fails loudly on drift.

## Known bench risks (verify first — see README "Verification status")
- No SPI slave partner exists for some buses yet; a transaction to an empty
  slot reads pad bytes (harmless). Whether SPI3 drains a quad read at speed is
  unproven. The head render-feed over Bus A is parked (status model exists; the
  serializer to `FH_PEER_HEAD` is a later pass).
- SDMMC slot default vs the TF slot; LDO4 power — see `fh_hub_board.c` D1/D5.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never builds/flashes here — Joshua
builds; Kiro reads build logs and probes the serial port read-only. "Compiles"
means compile-correct-by-reading with residual IDF/bench unknowns NAMED. Docs
travel with the edit; the README "Verification status" section must stay honest
(proven-on-hardware vs proven-by-reading vs unproven).
