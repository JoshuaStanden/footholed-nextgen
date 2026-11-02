# AGENTS.md — remix-fh-pico-cc1101 (for AI / LLM agents)

Read this BEFORE editing this blade. The repo-root `../AGENTS.md` governs the
whole rig (RP2040 MODE-1 rule, no-compile-here, flash procedure) — read it
first; this file carries only what is specific to the CC1101 blade.

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
2. **Two SPI blocks, never merged:** radio = SPI0 **master** to the CC1101 chip;
   hub = SPI1 **slave**. They share no pin. Do not route the hub link onto SPI0.
3. **IRQ-time callbacks stay tiny** (arduino-pico SPISlave contract): `hub_on_recv`
   only copies raw bytes to the SPSC buffer + sets a flag; `hub_on_sent` only
   re-presents the loop-built `s_hub_tx`. All decode/encode/staging happens in
   `loop()` (`service_rx` / `on_hub_frame` / `service_pending` / `service_stream`).
   No alloc, no locking, no SPI in a callback. The blade is single-core, so the
   mutex was removed — do not reintroduce one.
4. **The response is staged for the NEXT transaction.** A SPI slave cannot answer
   within the same clocked frame that delivered the request. The blade stages
   the reply and keeps re-presenting it; the hub polls with idle frames until the
   matching seq arrives. NEVER have the hub re-send the REQ to poll — that
   re-runs a completed capture. (Verified against this source, not assumed.)
5. **Radio core is carried verbatim** from the proven gen1 blade (timing table,
   `gdo0ISR`, `decodeSignal`, `transmitCode`, `radioEnterRx/Tx`). The risk
   surface is the hub-link transport, not the radio logic. Do not "improve" the
   decode without a bench.
6. **A hand-assembled PIO edge-capture draft was REJECTED** — do not resurrect it.

## Known bench risks (cannot be resolved by reading)
- **arduino-pico `SPISlave1` transmit may not stream 261 B reliably as a slave**
  even in MODE 1. This is the exact layer that FAILED on the GPS blade and forced
  its rewrite to the native pico-sdk pump. If this blade shows byte-0-then-0xff
  after MODE 1 is confirmed, the fix is the pico-sdk port — see `FUTURE.md`.
- **Library version:** pin an ELECHOUSE-compatible SmartRC-CC1101 release
  (SmartRC went V3.0.0 and bit the project before).
- **RSSI:** the RESP carries `ELECHOUSE_cc1101.getRssi()` — verify the method
  name on your driver version; worst case the field reverts to 0 (honest).

## Review discipline
This blade is subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes
here — "compiles" means compile-correct-by-reading, with residual library/bench
unknowns named, never claimed resolved. Docs travel with the edit: a code change
to the transport updates this file and `CONTINUITY.md` in the same pass.
