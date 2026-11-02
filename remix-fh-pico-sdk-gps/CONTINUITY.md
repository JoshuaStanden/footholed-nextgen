# remix-fh-pico-sdk-gps — CONTINUITY (resume point)

> Resume here if a session drops. Snapshot 2026-11-01. Folder
> `~/Desktop/remix footholed/`. Standing rules in the P4 hub CONTINUITY +
> `../AGENTS.md`. Per-part docs: WIRING / README / AGENTS / this / FUTURE
> (+ `PICO_LESSONS.md`, kept under its own name).

## What this is
Pico (RP2040) + NEO-6M GPS, native Pico C/C++ SDK. A polled SPI SLAVE on the P4
hub's Bus B, binary fh_protocol. **THE bench-proven reference blade.**

## Status: WORKING (2026-11-01)
- MODE 1 261-byte SPI stream, CRC-valid `GPS_FIX` (0x02) frames.
- Hub RTC locks from a blade frame; `fh_ui` shows `PICO=UP`, `RTC=ok`.
- Two bench bugs found and fixed IN CODE earlier (do not re-open): the RMC
  field-walk (`rmc_valid` was never true → RTC never locked) and the longitude
  3-digit-degree decode (was hardcoded 2-digit → ~17° error).
- The MISO/MOSI reconcile (2026-11-01) was **docs-only** — the pin macros
  (`FH_PICO_SPI_MOSI=16` / `FH_PICO_SPI_MISO=19`) were already the working,
  bench-confirmed values; only prose/comments lagged and were corrected.

## DO NOT edit the code
This blade is proven. Changes here are documentation. See `AGENTS.md`.

## Why it exists as native SDK (not Arduino)
The Arduino `SPISlave` transmit path could not stream reliably as a slave on the
bench (the RP2040 one-byte-per-CS behaviour + the callback model). The rewrite to
the native SDK with a hand-written pump + MODE 1 is what made it work. This is
now the template for porting the three radio blades if `SPISlave1` fails there.

## Open (bench, non-blocking — residuals, not bugs)
- NEO-6M clones may stay 1 Hz (UBX 5 Hz ignored) — fine, NMEA parses.
- `FH_GPS_SIM=1` is indoor-bench only.

## Not owed
- No rewrite for GPS itself. Its forward role is to stay the reference; see
  `FUTURE.md`.
