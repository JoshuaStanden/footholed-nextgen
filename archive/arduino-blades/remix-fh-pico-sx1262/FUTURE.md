# remix-fh-pico-sx1262 — FUTURE (planning)

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
1. New native-SDK project `remix-fh-pico-sdk-sx1262/` mirroring the GPS layout:
   `main.c`, `CMakeLists.txt` (`PICO_NO_PICOTOOL=1`), `newlib_lock_stubs.c`,
   `uf2conv.py`, byte-identical `fh_frame.c/.h` + `fh_ondemand.h`.
2. **The RadioLib shim is the real work.** RadioLib is an Arduino/C++ library;
   it is NOT available on the bare Pico SDK. The SX1262 needs more than a few
   register pokes (begin with TCXO, SF/BW/CR/sync config, transmit(), startReceive
   / checkDataReady / readData, getRSSI/getSNR, RF_SW1). Options: (a) build
   RadioLib against the SDK as a C++ component if it compiles standalone, (b)
   port the specific SX126x command sequences this blade uses to raw SDK SPI0,
   or (c) keep a thin SX126x driver. This is materially harder than the CC1101
   (ELECHOUSE) or nRF24 (RF24) shims — weigh it before committing.
3. **Replace the `SPISlave1` hub link with the GPS pump:** copy GPS's
   `spi_reset_and_preload` + `spi_txn_pump` + `spi_after_txn` + CS-edge ISR
   verbatim; MODE 1; 261-byte transactions; stage the reply for the next
   transaction. The ~1 s LoRa TX and the listen poll run in the core loop, not
   the pump, so the bus never stalls (mirror the GPS core0/foreground split).
4. Keep the `fh_ondemand.h` LoRa verb handling (PING/LORA_TX/LORA_LISTEN/
   RX_IND) identical; only transport + the RadioLib shim change.
5. Build/flash via the GPS recipe.

**Committed (2026-11-01):** ALL Pico blades port to the native pico-sdk — decided,
not conditional. Order is **cc1101 (pilot) → nrf24 → sx1262 (this one, LAST)**.
sx1262 is last on purpose: the RadioLib-on-SDK shim is materially harder than the
cc1101/nrf24 shims, so prove the pattern on those first. A pre-port bench run on
this (now MODE 1) arduino version is a useful data point but does not gate the
port. See `../remix-PICO-SDK-PORT-PLAN.md`.

## 2. Smaller forward items
- LoRa-as-capture (listening to the RF environment with LoRa) is deferred, not
  ruled out — add a sniff verb/intent if/when it becomes a feature.
- The hub-side RX_IND drain during a listen window needs a bench proof.
