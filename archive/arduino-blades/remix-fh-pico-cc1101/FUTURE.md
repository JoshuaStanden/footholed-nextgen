# remix-fh-pico-cc1101 — FUTURE (planning)

Forward-looking notes for this blade. Nothing here is built; it is the plan a
future session (or the other LLM) picks up.

## 1. The pico-sdk transport rewrite (the big one)

**Why it may be needed.** The GPS blade was originally an arduino-pico sketch
using `SPISlave`. On the bench it could not reliably transmit as a slave — the
RP2040 SSP slave one-byte-per-CS behaviour plus the `SPISlave` callback model.
It was rewritten to the **native Raspberry Pi Pico C/C++ SDK** with a
hand-written full-duplex pump (full SPI0 peripheral reset + 8-byte FIFO preload
in the idle gap, CS-edge tracking to realign), MODE 1, and it is now bench-proven
(261-byte stream, CRC-valid frames). See `../remix-fh-pico-sdk-gps/`
(`main.c`, `CMakeLists.txt`, `uf2conv.py`) and its `PICO_LESSONS.md`.

The three radio blades (this one, nrf24, sx1262) still ride the **same
arduino-pico `SPISlave1` path that failed on GPS.** The MODE-1 fix (2026-11-01)
is necessary and correct, but may not be sufficient: if this blade shows
byte-0-then-0xff after MODE 1 is confirmed on the bench, the durable answer is
the pico-sdk port.

**Port plan (per blade, GPS is the reference):**
1. New native-SDK project `remix-fh-pico-sdk-cc1101/` mirroring the GPS layout:
   `main.c`, `CMakeLists.txt` (with `PICO_NO_PICOTOOL=1`), `newlib_lock_stubs.c`,
   `uf2conv.py`, byte-identical `fh_frame.c/.h` + `fh_ondemand.h`.
2. **Carry the radio core verbatim** — the ELECHOUSE CC1101 calls, the RCSwitch
   timing table, `gdo0ISR` capture ring, `decodeSignal`, `transmitCode`. The
   radio talks SPI0 as master; that is unchanged. (NOTE: on the SDK the ELECHOUSE
   Arduino library is not available — either port the handful of register writes
   it does to raw SDK SPI, or keep a thin shim. This is the real work of the port
   and is why it is a plan, not a one-liner.)
3. **Replace the `SPISlave1` hub link with the GPS pump:** copy the GPS blade's
   `spi_reset_and_preload` + `spi_txn_pump` + `spi_after_txn` + CS-edge ISR
   verbatim; MODE 1; 261-byte transactions; stage the reply for the next
   transaction (same contract). The long capture runs in the core loop, not the
   pump, so it never stalls the bus — mirror the GPS core0/foreground split.
4. Keep the `fh_ondemand.h` CC1101 verb handling (PING/CC1101_REQ/RAW_OOK/REPLAY
   /EXT TX) exactly as the arduino version implements it; only the transport and
   the radio-library shim change.
5. Build/flash via the GPS recipe (toolchain exports, `uf2conv.py`,
   PowerShell Copy-Item + eject).

**Committed (2026-11-01):** ALL Pico blades port to the native pico-sdk — this
is decided, not conditional on a bench failure. **cc1101 is the PILOT** (simplest
radio shim, deepest capability): port it FIRST, prove the pattern, then nrf24,
then sx1262. A pre-port bench run on this (now MODE 1) arduino version is a useful
data point but does not gate the port. See `../remix-PICO-SDK-PORT-PLAN.md`.

## 1a. The radio-library shim is SOLVED (vetted driver found 2026-11-01)

The hard half of the port — reimplementing ELECHOUSE as raw register writes — is
avoided. A native Pico-SDK fork of the exact SmartRC/ELECHOUSE driver the blade
already calls exists and was vetted by reading:
**`github.com/sentoxo/SmartRC-CC1101-Driver-Lib-Pico`** (fork of
`LSatan/SmartRC-CC1101-Driver-Lib`).

Vetting verdict (3 passes, read-only):
- **Provenance/safety CLEAN.** Byte-for-byte the canonical driver (register map,
  strobes, PA tables, setMHZ/Calibrate math, RC-Switch TX/RX). The ONLY delta
  from upstream is the hardware layer: Arduino `SPI`/`digitalWrite`/`delay` →
  SDK `spi_write_blocking`/`spi_write_read_blocking`/`gpio_put`/`gpio_get`/
  `sleep_ms`. No network, no shell-out, no file I/O, no obfuscation — pure
  SPI + GPIO. Keep the MIT attribution.
- **API CLEAN drop-in.** Every call the blade makes (`Init`, `setSpiPin`,
  `setGDO`, `setCCMode`, `setModulation`, `setMHZ`, `SetRx/SetTx`, `getRssi`,
  `setSidle`, `setPA`, `getCC1101`) exists with matching signatures.

NOT a zero-edit drop-in — three integration findings to handle when porting:

1. **SPI block split — DECIDED 2026-11-01: radio = spi1, hub link = spi0.**
   The Pico has two SPI blocks; the radio driver defaults to spi0 and the proven
   GPS hub pump also uses spi0, so they collide and one must move. The **hub
   pump stays on spi0** because it is the bench-proven piece (leave the proven
   thing alone); the **radio driver moves to spi1** — change its
   `#define SPI_BUS spi0` to `spi1` and wire the CC1101 to spi1-capable pins.
   This is a planning decision recorded here; applying it is part of the port.
2. **`getCC1101()` calls `setSpi()`, which OVERWRITES custom pins** back to the
   driver defaults (SCK18/MISO16/MOSI19/**SS2**). The blade sets `setSpiPin(...)`
   with CSN=GP17 then calls `getCC1101()` → the custom SS gets clobbered to GP2.
   Fix: guard `getCC1101()` so it does not re-run `setSpi()` when the `spi` flag
   shows custom pins were set (the driver already tracks that flag; `getCC1101`
   just ignores it). Carry this fix in our copy.
3. **Driver SPI mode = MODE 0 (CPOL0/CPHA0), 4 MHz — CORRECT, do not "fix".**
   This is the RADIO-FACING SPI (Pico = MASTER to the CC1101 chip, a MODE-0
   device). It is UNRELATED to the hub-facing MODE-1 slave link (where the Pico
   is a slave to the hub). Two different SPI roles; both are right. Do NOT change
   this to MODE 1 — the MODE-1 rule is about the hub link only.

Residual (honest): it is a 0-star fork, vetted by reading only; its on-silicon
correctness is itself a bench item. But the reading is clean and the deltas from
upstream are exactly and only the SDK port.

## 2. Smaller forward items
- REPLAY of a NAMED stored capture (not just the last one) needs the Medusa-side
  dump store + `cc1101_replay(dump_id)` — Layer-2 intent, host side.
- PIO edge capture was rejected in v0; if timing precision ever demands it, it is
  a deliberate later optimization, not a bug fix.
