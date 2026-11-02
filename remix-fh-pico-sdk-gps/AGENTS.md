# AGENTS.md — remix-fh-pico-sdk-gps (for AI / LLM agents)

Read this BEFORE touching this blade. The repo-root `../AGENTS.md` governs the
whole rig; `PICO_LESSONS.md` (in this folder) is the crown-jewel RP2040 reference
— read both. This file carries what is specific to the GPS blade.

> **This folder holds the MASTER `PICO_LESSONS.md`.** Verbatim copies live in
> the three Arduino Pico blades (cc1101, nrf24, sx1262) so each blade zip is
> self-contained. If you edit the lessons, edit them HERE and re-copy to every
> Pico blade; keep all copies byte-identical (md5 must match).

## ★ THE ONE RULE: GPS IS WORKING — DOCS ONLY

This blade is **bench-proven on hardware** (2026-11-01: MODE 1 261-byte stream,
CRC-valid `GPS_FIX`, RTC locked, `PICO=UP`). **Do NOT edit its code** — not
`main.c`, `fh_frame.c/.h`, `fh_ondemand.h`, `fh_pico_pins_sdk.h`,
`CMakeLists.txt`, `slave_min.c`, `newlib_lock_stubs.c`, `uf2conv.py`, or anything
under `releases/`. If a change seems needed, it is almost certainly a
DOCUMENTATION change. The working code is the ground truth; the docs serve it,
not the other way around. (The MISO/MOSI reconcile 2026-11-01 was docs-only —
the macros `FH_PICO_SPI_MOSI=16` / `FH_PICO_SPI_MISO=19` were already correct.)

## This blade is THE reference
Every other Pico blade copies its SPI transport from here. The things to
preserve/understand (not change):
1. **MODE 1 (CPOL=0, CPHA=1)** on the spi0 slave — the RP2040 one-byte-per-CS
   fix. Non-negotiable across the whole rig.
2. **The hand-written full-duplex pump** — `spi_reset_and_preload`
   (full SPI0 peripheral reset + 8-byte FIFO preload in the idle gap),
   `spi_txn_pump` (paired RX/TX push to 261 B), `spi_after_txn`, and the CS-edge
   ISR that realigns the byte counter. This replaced the Arduino `SPISlave`
   callback model, which could not stream reliably as a slave. This pump is the
   pattern the radio-blade pico-sdk ports copy.
3. **Dual-core split** — core1 GPS/NMEA + stage + `prepare_tx`; core0 the SPI
   pump. `prepare_tx` runs in core1 context (never an ISR), published under
   `s_stage_mtx`; core0 snapshots between transactions. Do not move encoding
   into an ISR.
4. **Liveness by frame** — the GPS blade slot carries `FH_SRC_PICO` so its 0x02
   frames mark it seen via `fh_health_on_frame`. A slot with src_id 0 reads `dn`
   forever (the trap).

## Known residuals (bench facts, not bugs)
- A NEO-6M clone may ignore the UBX 5 Hz bump — NMEA still parses at 1 Hz (the
  LED blinks the verdict at boot).
- `FH_GPS_SIM=1` is indoor-bench only; never ship a field rig with it.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes here — and for
THIS blade, Kiro never edits the code at all (it is proven). Docs are kept honest
and in step with the working firmware.
