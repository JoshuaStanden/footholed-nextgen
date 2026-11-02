# AGENTS.md — remix-fh-pico-sdk-cc1101 (for AI / LLM agents)

Read this BEFORE editing this blade. The repo-root `../AGENTS.md` governs the
whole rig (RP2040 MODE-1 rule, no-compile-here, flash procedure, the committed
all-blades-to-pico-sdk policy) — read it first; this file carries only what is
specific to the native-SDK CC1101 blade.

> **`PICO_LESSONS.md` is in this folder** (the RP2040 bench lessons: MODE 1, the
> pump design, flash + toolchain). It is a **verbatim copy of the master in
> `remix-fh-pico-sdk-gps/`** (md5 `a39f5c37212e6e9c584f5e5dcf2e5511`) — carried
> here so this blade's zip is self-contained (same discipline as the
> byte-identical `fh_frame.c/.h`). The GPS copy is the master: edit it there,
> then re-copy to every Pico blade; keep all copies byte-identical.

## What this blade IS
The pilot pico-sdk port of `../archive/arduino-blades/remix-fh-pico-cc1101/`. The hub-link transport is
the **bench-proven GPS pump copied verbatim**; the radio runs on a second SPI
block through a vetted SDK CC1101 driver. The arduino blade stays until this is
bench-proven, then retires (per the committed port plan).

## Non-negotiables (this blade)
1. **The hub pump is COPIED FROM THE PROVEN GPS BLADE — do not redesign it.**
   `spi_reset_and_preload` + `spi_txn_pump` + `spi_after_txn` are the GPS blade's
   working full-duplex 261-byte transaction (SPI0 reset, 8-byte FIFO preload in
   the idle gap, paired RX/TX push, CS-edge realign). If you touch it, you are
   touching the one thing on this blade that is known to work. Don't.
2. **Hub link = spi0 slave MODE 1 (CPOL=0, CPHA=1).** `spi_set_format(spi0, 8,
   SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST)`. A MODE-0 RP2040 SSP slave sends ONE
   byte per CS assertion — no preload fix helps. Do NOT set MODE 0.
3. **Two SPI blocks, never merged, and NOTE the split differs from the arduino
   blade:** hub = **spi0 slave**, radio = **spi1 master**. (The arduino blade had
   radio on SPI0, hub on SPI1 — inverted here so the hub pump stays on the proven
   spi0.) They share no pin.
4. **core0 is the SINGLE WRITER of the outbound frame.** Only core0 calls
   `stage_frame`. core1 computes radio results into the mailboxes
   (`s_resultbuf`/`s_result_ready`, the dump buffer/`s_stream_ready`,
   `s_ext_resultbuf`/`s_ext_tx_ready`) and core0 drains them. Do NOT call
   `stage_frame` from core1 — that reintroduces the lost-update race the
   multi-check removed.
5. **core0 NEVER touches spi1.** The PING_REQ `radios_ready` bit reads the cached
   `s_radio_ready` that core1 sets at init. A live `cc_present()` from core0
   during a core1 capture would be a cross-core spi1 collision.
6. **The driver is VENDORED + PINNED with exactly two FH-EDITs.** In
   `ELECHOUSE_CC1101_SRC_DRV.cpp`: `#define SPI_BUS spi1` (was spi0, collided
   with the hub pump), and the `getCC1101()` guard `if (spi == 0) { setSpi(); }`
   (stops it re-running `setSpi()` and clobbering our custom pins to SS=GP2).
   Both are marked FH-EDIT. Do not re-vendor from upstream without re-applying
   them, and do not "improve" the driver — it is vetted as-is.
7. **main.c is compiled as C and MUST NOT include the driver's C++ class
   header.** It reaches the driver ONLY through the `cc_*` extern-"C" wrappers in
   `cc_shim.cpp`. (A C compiler cannot parse `class` — this was a fixed bug.)
8. **The response is staged for the NEXT transaction.** A SPI slave cannot answer
   within the frame that delivered the request. NEVER have the hub re-send a REQ
   to poll — that re-runs a completed capture.
9. **Radio core carried from the proven gen1/arduino blade** (timing table,
   capture ring, `decode_signal`, the OOK transmit in `cc_shim.cpp`). Adapted to
   SDK timing (`time_us_32`, gpio IRQ). Do not "improve" the decode without a
   bench.

## Known bench risks (cannot be resolved by reading — see main.c KNOWN RISKS)
- **R-LINK:** mixed C/C++ link (main.c C → driver C++ via the shim). Confirm on
  the first build.
- **R-SPI2:** the GDO0 capture IRQ shares ONE GPIO-bank callback with the hub CS
  IRQ, running on core0; GDO0 edges are enabled only during a capture window.
  Tolerable because the hub polls (a missed CS just retries). If the bench shows
  pump starvation, escalate to `irq_set_exclusive_handler(IO_IRQ_BANK0)` on
  core1 — documented, NOT done.
- **Driver on silicon:** SmartRC-CC1101-Driver-Lib-Pico is a 0-star fork; vetted
  by reading (provenance + API clean), not bench-run. The spi1 bring-up, GDO0
  capture, and RSSI (`getRssi`) are the unproven surface.
- **RAW_OOK ingest:** verify the hub ingests the `FH_OD_DUMP_IND` chunk-then-RESP
  ordering and that one-chunk-per-transaction pacing keeps up.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes here —
"compiles" means compile-correct-by-reading, residual unknowns named, never
claimed resolved. Docs travel with the edit: a transport change updates this
file and `CONTINUITY.md` in the same pass.
