# remix-PICO-SDK-PORT-PLAN.md — porting the radio blades to the native Pico SDK

> Repo-level plan. The three arduino-pico RADIO blades (cc1101, nRF24, sx1262)
> may need to be rewritten to the native Raspberry Pi Pico C/C++ SDK, the same
> way the GPS blade already was. This gathers the per-blade `FUTURE.md` port
> notes in one place. Nothing here is built. Honest about what is unproven.

## 1. Why this may be needed

The GPS blade was originally an arduino-pico sketch using the `SPISlave` library.
Through ~4 hours of "nothing works" bench debugging two problems surfaced, in
order:

1. **MISO/MOSI were wired reversed** (the first, masking bug — since fixed; the
   working values are the ones now in the GPS docs).
2. **With wiring correct, the Pico still could not transmit reliably as a
   `SPISlave`** — the RP2040 SSP slave transmits only ONE byte per CS assertion
   in MODE 0 (freezes the shift register), and the Arduino `SPISlave` callback
   model did not stream the full frame. Changing the mode alone was not enough
   through that library.

The fix was to **abandon `SPISlave` and rewrite the GPS blade on the native Pico
SDK** with a hand-written full-duplex pump and MODE 1. That blade is now
bench-proven (261-byte stream, CRC-valid frames, RTC lock).

The three radio blades still ride the **same arduino-pico `SPISlave1` path that
failed on GPS.** Their SPI mode has been corrected to MODE 1 (2026-11-01), which
is necessary and removes a definite defect — but the GPS history is direct
evidence that MODE 1 alone may not be **sufficient** on `SPISlave1`. So:

> **DECISION (2026-11-01, Joshua): ALL Pico blades are ported to the native
> pico-sdk.** This is a committed policy, not a per-blade "only if it fails"
> fork. GPS is already native SDK; cc1101, nrf24, and sx1262 follow, in that
> order. MODE 1 on the arduino versions stays correct in the meantime (so a
> bench run before the port is not wasted), but the destination for every Pico
> blade is the SDK + the GPS pump — we are not keeping the `SPISlave1` transport.

> **ORDER UPDATE (2026-11-01): sx1262 pulled ahead of nrf24.** The committed
> order was cc1101 (pilot) → nrf24 → sx1262, with sx1262 last because the
> RadioLib-on-SDK shim was expected hardest. cc1101 was ported first (done),
> then **sx1262 was promoted** because LoRa is the critical-path comms link to
> the sensor stick (still being built). The RadioLib shim proved tractable via
> RadioLib's own in-tree Pico-SDK HAL (`PicoHal`), so "hardest, do last" no
> longer gated it. Both cc1101 and sx1262 SDK ports now exist as first drafts;
> **nrf24 is the remaining port** (RF24 library — no obvious SDK drop-in yet;
> investigate the driver before writing, as we did for RadioLib).

> **ALL THREE RADIO PORTS DONE (2026-11-01):** nrf24 is now ported too. The
> clean pico-sdk nRF24 driver (AndyRids/pico-nrf24) is GPL-2.0, which would
> force the otherwise-permissive project to GPL, so it was rejected; instead
> `remix-fh-pico-sdk-nrf24/nrf_shim.c` is a hand-written, FH-owned, permissive
> minimal nRF24 driver against the Nordic datasheet. cc1101, sx1262, nrf24 + GPS
> now all exist as native-SDK first drafts. Remaining program item: retire the
> arduino blades once the SDK ports are bench-proven (owner decision pending).

## 2. The reference: what the GPS SDK blade did

See `remix-fh-pico-sdk-gps/` (`main.c`, `CMakeLists.txt`, `uf2conv.py`) and its
`PICO_LESSONS.md`. The transport pattern to copy:

- **MODE 1** spi0 slave: `spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1,
  SPI_MSB_FIRST)`.
- **`spi_reset_and_preload`** — a FULL SPI0 peripheral reset (`reset_block` /
  `unreset_block_wait`) then re-arm (`spi_init` + `spi_set_slave`) and preload 8
  bytes into the TX FIFO, run ONLY in the idle gap between the hub's two
  transactions.
- **`spi_txn_pump`** — paired RX/TX push: one TX byte pushed per RX byte clocked,
  exactly 261, with a CS-edge ISR (`cs_gpio_isr`) tracking CS fall/rise to
  complete the transaction and realign the byte counter; a 200 ms watchdog so the
  pump can never wedge.
- **`spi_after_txn`** — between transactions, decode the inbound bytes and
  snapshot the staged outbound frame for the next transaction.
- **Dual-core split** — the radio/sensor work + frame build in the core loop
  (or core1), the SPI pump in core0; encoding never in an ISR.
- **Build flow** — `PICO_NO_PICOTOOL=1`, `newlib_lock_stubs.c` (pqt-gcc newlib
  retarget locks), `.bin` → `.uf2` via `uf2conv.py`; flash with PowerShell
  `Copy-Item` + eject (a bare `cp` does not flash).

## 3. Per-blade plan

For each blade that needs it, create `remix-fh-pico-sdk-<radio>/` mirroring the
GPS layout, carry the fh_ondemand verb handling verbatim, and swap the transport
for the GPS pump. The **radio-library shim is the real work**, because the
Arduino radio libraries are not available on the bare SDK:

- **cc1101** (`remix-fh-pico-sdk-cc1101/`): the radio core is ELECHOUSE
  SmartRC-CC1101 calls + the RCSwitch timing table + the GDO0 edge-IRQ capture
  ring + `transmitCode`. **The library shim is SOLVED** — a vetted native
  Pico-SDK fork of the exact ELECHOUSE driver exists
  (`github.com/sentoxo/SmartRC-CC1101-Driver-Lib-Pico`, fork of
  `LSatan/SmartRC-CC1101-Driver-Lib`). Vetted by reading 2026-11-01: clean
  provenance (only the hardware layer is ported to SDK SPI/GPIO), full API match
  to the blade's calls. NOT zero-edit — three integration findings (see
  `archive/arduino-blades/remix-fh-pico-cc1101/FUTURE.md` §1a for detail): (1) driver hard-codes
  `#define SPI_BUS spi0`, which collides with the hub pump also on spi0 —
  **DECIDED 2026-11-01: radio = spi1, hub pump = spi0** (keep the proven pump
  where it is; move the radio to spi1 + wire CC1101 to spi1 pins); (2)
  `getCC1101()` re-runs
  `setSpi()` and clobbers custom pins back to SS=GP2 — guard it so our CSN=GP17
  survives; (3) driver SPI mode is MODE 0 — CORRECT (radio-facing master to a
  MODE-0 chip), unrelated to and must NOT be confused with the hub-facing MODE 1.
  The capture IRQ (`gdo0ISR`) uses `micros()` → `time_us_32()`/`absolute_time` on
  the SDK. **Easiest of the three** and holds the deepest capability — PILOT.
  (Residual: 0-star fork, vetted by reading only; on-silicon correctness is a
  bench item.)
- **nRF24** (`remix-fh-pico-sdk-nrf24/`): the RF24 (TMRh20) library is Arduino —
  port the operations this blade uses (setChannel, RPD/carrier test,
  start/stopListening, dynamic-payload read, write) to raw SDK SPI0, or a thin
  shim. Medium effort.
- **sx1262** (`remix-fh-pico-sdk-sx1262/`): RadioLib is a larger Arduino/C++
  library (begin+TCXO, SF/BW/CR/sync, transmit, startReceive/checkDataReady/
  readData, getRSSI/getSNR, RF_SW1). The shim is materially harder — either build
  RadioLib standalone against the SDK if it compiles, or port the SX126x command
  sequences. **Port it LAST**, after the pattern is proven on cc1101/nrf24.

All three keep the SPI0-master-to-radio / SPI1-equivalent-slave-to-hub split; on
the SDK the "SPI1 slave" becomes the same spi0-pump pattern GPS uses (GPS uses
spi0 for the hub link and has no second radio SPI — a radio blade has the radio
on one spi block as master and the hub pump on the other; mirror GPS's pump on
the hub-link block).

## 4. Committed plan (decided 2026-11-01)
- **ALL three radio blades port to the native pico-sdk.** Not conditional on a
  bench failure — the arduino-pico `SPISlave1` transport is being retired across
  the board; the SDK + GPS pump is the destination for every Pico blade.
- **Order: cc1101 (pilot) → nrf24 → sx1262 (hardest shim).** cc1101 proves the
  pattern on the simplest radio shim and holds the deepest capability; sx1262 is
  last because the RadioLib-on-SDK shim is the real work.
- Each ported blade is a NEW `remix-fh-pico-sdk-<radio>/` package. The arduino
  version stays until its SDK replacement is **bench-proven**, then is retired —
  one flashable unit per blade, independent lifecycle, no dangling old copy once
  the new one is proven.
- A pre-port bench run on the arduino version (now MODE 1) is still worthwhile as
  a data point, but it does NOT gate the port — the port happens regardless.

Cross-references: the active blade docs are now `remix-fh-pico-sdk-cc1101/`,
`remix-fh-pico-sdk-nrf24/`, `remix-fh-pico-sdk-sx1262/` (each has its own
FUTURE/CONTINUITY/AGENTS/WIRING/README); `remix-fh-pico-sdk-gps/` (the
reference); `remix-BUILD-TOOLCHAIN.md` (how to build them); `AGENTS.md` §1 (the
MODE-1 rule). The superseded arduino blades are archived under
`archive/arduino-blades/` (fallback until the SDK ports are bench-proven).
