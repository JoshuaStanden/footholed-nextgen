# AGENTS.md — remix-fh-pico-sx1262 (for AI / LLM agents)

Read this BEFORE editing this blade. The repo-root `../AGENTS.md` governs the
whole rig (RP2040 MODE-1 rule, no-compile-here, flash procedure) — read it
first; this file carries only what is specific to the SX1262 LoRa blade.

> **`PICO_LESSONS.md` is in this folder** (the RP2040 bench lessons: MODE 1, the
> pump design, flash + toolchain). It is a **verbatim copy of the master in
> `remix-fh-pico-sdk-gps/`** — carried here so this blade's zip is self-contained
> (same discipline as the byte-identical `fh_frame.h`). The GPS copy is the
> master: edit it there, then re-copy to every Pico blade; keep all copies
> byte-identical (md5 must match).

## Non-negotiables (this blade)
1. **Hub-link SPI slave MUST be MODE 1 (CPOL=0, CPHA=1).** `SPISlave1.begin(
   SPISettings(8000000, MSBFIRST, SPI_MODE1))`. A MODE 0 RP2040 SSP slave sends
   ONE byte per CS assertion, then 0xff underrun — no pump/preload fix helps.
   The hub drives Bus B in MODE 1; the bench-proven GPS blade is MODE 1. Do NOT
   revert to SPI_MODE0 (corrected 2026-11-01; see root `../AGENTS.md` §1 and
   `../remix-fh-pico-sdk-gps/PICO_LESSONS.md` §1).
2. **Two SPI blocks, never merged:** radio = SPI0 **master** to the SX1262; hub
   = SPI1 **slave** on the UPPER spi1 group (GP14/24/15/25). The radio pins block
   the lower spi1 group — the old GP6/4/3/5 draft was an SPI0 group and would
   never clock (fix 2026-10-31). Do not route the hub link onto SPI0.
3. **IRQ-time callbacks stay tiny** (arduino-pico SPISlave contract): `hub_on_recv`
   copies raw bytes only; `hub_on_sent` re-presents the loop-built `s_hub_tx`.
   Decode/encode/staging + the LoRa listen poll happen in `loop()`
   (`service_rx`/`on_hub_frame`/`service_pending`/`service_listen`). No alloc,
   no locking, no SPI in a callback. Single-core — mutex removed; don't re-add.
4. **Response staged for the NEXT transaction**; the hub polls with idle frames
   until the matching seq. Never re-send the REQ to poll.
5. **RadioLib bring-up carried verbatim** (CORE-2): explicit `begin()` with
   `tcxoVoltage = 1.6` enables the Wio module's TCXO on DIO3; `Module(cs,dio1,
   rst,busy)` pin order is the official order. RF_SW1 (GP9) HIGH=TX, LOW=RX —
   VERIFY the sense on your module rev.
6. **Non-blocking RX only.** `service_listen()` gates on `checkDataReady()`
   BEFORE `readData()`. A blocking `receive()` would hang the command loop
   (gen1 CORE-4) — do NOT use it. Verify readData/getPacketLength/getRSSI/getSNR
   names against RadioLib 7.8.1.
7. **LoRa is COMMS, not capture.** TX is authorized-use-only, gated on the
   hub/head. LoRa-as-capture is deferred, not ruled out — do not add it here.

## Known bench risks (cannot be resolved by reading)
- **arduino-pico `SPISlave1` transmit may not stream 261 B reliably as a slave**
  even in MODE 1. This is the exact layer that FAILED on the GPS blade and forced
  its rewrite to the native pico-sdk pump. If this blade shows byte-0-then-0xff
  after MODE 1 is confirmed, the fix is the pico-sdk port — see `FUTURE.md`.
- RadioLib begin() codes: `-707` TCXO/power, `-2` SPI/DIO wiring.
- RF_SW1 sense; antenna MUST be attached before any TX (+22 dBm PA).

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes here —
"compiles" means compile-correct-by-reading, residual library/bench unknowns
named, never claimed resolved. Docs travel with the edit: a transport code
change updates this file and `CONTINUITY.md` in the same pass.
