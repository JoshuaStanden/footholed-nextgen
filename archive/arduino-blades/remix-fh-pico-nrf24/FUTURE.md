# remix-fh-pico-nrf24 — FUTURE (planning)

Forward-looking notes for this blade. Nothing here is built; it is the plan a
future session (or the other LLM) picks up.

## 1. The pico-sdk transport rewrite (the big one)

**Why it may be needed.** The GPS blade was originally an arduino-pico sketch
using `SPISlave`. On the bench it could not reliably transmit as a slave — the
RP2040 SSP slave one-byte-per-CS behaviour plus the `SPISlave` callback model.
It was rewritten to the **native Raspberry Pi Pico C/C++ SDK** with a
hand-written full-duplex pump (full SPI0 peripheral reset + 8-byte FIFO preload
in the idle gap, CS-edge tracking to realign), MODE 1, and it is now bench-proven.
See `../remix-fh-pico-sdk-gps/` and its `PICO_LESSONS.md`.

This blade still rides the **same arduino-pico `SPISlave1` path that failed on
GPS.** The MODE-1 fix (2026-11-01) is necessary and correct, but may not be
sufficient: if this blade shows byte-0-then-0xff after MODE 1 is confirmed on
the bench, the durable answer is the pico-sdk port.

**Port plan (GPS is the reference):**
1. New native-SDK project `remix-fh-pico-sdk-nrf24/` mirroring the GPS layout:
   `main.c`, `CMakeLists.txt` (with `PICO_NO_PICOTOOL=1`), `newlib_lock_stubs.c`,
   `uf2conv.py`, byte-identical `fh_frame.c/.h` + `fh_ondemand.h`.
2. **Carry the radio bring-up verbatim** — but note the **RF24 (TMRh20) library
   is an Arduino library**, not available on the bare SDK. This is the real work
   of the port: either port the handful of RF24 register operations this blade
   uses (channel set, RPD/carrier test, startListening/stopListening, dynamic
   payload read, write) to raw SDK SPI0, or keep a thin RF24-shim. The nRF24
   talks SPI0 as master; that role is unchanged.
3. **Replace the `SPISlave1` hub link with the GPS pump:** copy the GPS blade's
   `spi_reset_and_preload` + `spi_txn_pump` + `spi_after_txn` + CS-edge ISR
   verbatim; MODE 1; 261-byte transactions; stage the reply for the next
   transaction. The scan/sniff dwell runs in the core loop, not the pump.
4. Keep the `fh_ondemand.h` nRF24 verb handling (PING/NRF24_REQ scan+sniff/EXT
   TX) exactly as the arduino version implements it; only transport + the RF24
   shim change.
5. Build/flash via the GPS recipe (toolchain exports, `uf2conv.py`, PowerShell
   Copy-Item + eject).

**Committed (2026-11-01):** ALL Pico blades port to the native pico-sdk — decided,
not conditional. Order is **cc1101 (pilot) → nrf24 (this one, second) → sx1262**.
A pre-port bench run on this (now MODE 1) arduino version is a useful data point
but does not gate the port. See `../remix-PICO-SDK-PORT-PLAN.md`.

## 2. Smaller forward items
- Real capture/sniffer modules are a deliberate later, head-gated layer — not a
  framework gap.
- The hub-side requester → pico-nrf24 CSV row round trip needs a bench proof.
