# AGENTS.md — remix-fh-pico-nrf24 (for AI / LLM agents)

Read this BEFORE editing this blade. The repo-root `../AGENTS.md` governs the
whole rig (RP2040 MODE-1 rule, no-compile-here, flash procedure) — read it
first; this file carries only what is specific to the nRF24 blade.

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
2. **Two SPI blocks, never merged:** radio = SPI0 **master** to the nRF24 chip;
   hub = SPI1 **slave**. They share no pin. Do not route the hub link onto SPI0.
3. **IRQ-time callbacks stay tiny** (arduino-pico SPISlave contract): `hub_on_recv`
   only copies raw bytes to the SPSC buffer + sets a flag; `hub_on_sent` only
   re-presents the loop-built `s_hub_tx`. All decode/encode/staging happens in
   `loop()`. No alloc, no locking, no SPI in a callback. Single-core — the mutex
   was removed; do not reintroduce one.
4. **The response is staged for the NEXT transaction** (SPI slave cannot answer
   within the delivering frame). The hub polls with idle frames until the
   matching seq arrives — NEVER re-send the REQ to poll (it re-runs the op).
5. **nRF24 has NO true RSSI** (carried CORE-3 lesson) — only 1-bit RPD. The scan
   reports `active_chan`; `resp.rssi` stays 0. Never invent a dBm. `testRPD()` is
   P-variant; `testCarrier()` is the non-P fallback — the code tries both.
6. **RF24 bring-up carried verbatim** incl. `radio.begin(&SPI)` pin-binding (the
   H1 false-absent fix). The risk surface is the hub-link transport, not RF24.
7. **FRAMEWORK ONLY** — no capture/sniffer/attack modules. The scan is a presence
   probe. Modules are later, head-gated. Do not add offensive modules here.

## Known bench risks (cannot be resolved by reading)
- **arduino-pico `SPISlave1` transmit may not stream 261 B reliably as a slave**
  even in MODE 1. This is the exact layer that FAILED on the GPS blade and forced
  its rewrite to the native pico-sdk pump. If this blade shows byte-0-then-0xff
  after MODE 1 is confirmed, the fix is the pico-sdk port — see `FUTURE.md`.
- **Missing decoupling cap** is the classic false `radio=ABSENT` (10µF at VCC).
- `testRPD()`/`testCarrier()` names + the RPD threshold vary by RF24 version.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes here —
"compiles" means compile-correct-by-reading, residual library/bench unknowns
named, never claimed resolved. Docs travel with the edit: a transport code
change updates this file and `CONTINUITY.md` in the same pass.
