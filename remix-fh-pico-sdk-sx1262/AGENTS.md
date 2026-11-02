# AGENTS.md — remix-fh-pico-sdk-sx1262 (for AI / LLM agents)

Read this BEFORE editing this blade. The repo-root `../AGENTS.md` governs the
whole rig (RP2040 MODE-1 rule, no-compile-here, flash procedure, committed
all-blades-to-pico-sdk policy). This file carries only the sx1262-specific part.

> **`PICO_LESSONS.md` is in this folder** — a **verbatim copy of the master in
> `remix-fh-pico-sdk-gps/`** (md5 `a39f5c37212e6e9c584f5e5dcf2e5511`). Edit the
> GPS master, then re-copy; keep all copies byte-identical.

## What this blade IS
The pico-sdk port of `../archive/arduino-blades/remix-fh-pico-sx1262/`. Hub transport = the proven GPS
pump (spi0); radio = RadioLib + its official Pico-SDK HAL (PicoHal) on spi1.
LoRa is the COMMS link to the sensor stick (TX is core), not a capture radio.
The arduino blade stays until this is bench-proven, then retires.

## Non-negotiables
1. **The hub pump is COPIED FROM THE PROVEN GPS BLADE — do not redesign it.**
   The ONE sanctioned change is that CS is POLLED (`cs_asserted()`), not an
   edge IRQ — see R-IRQBANK below. Everything else (full SPI0 reset + 8-byte
   preload + paired RX/TX push per 261-byte transaction) is the GPS design.
2. **Hub link = spi0 slave MODE 1.** Radio = spi1 master MODE 0 (PicoHal sets
   it). Two blocks, never merged. (Split is inverted vs the arduino blade.)
3. **R-IRQBANK — the central design call, do not undo it.** RadioLib's PicoHal
   installs a shared `IO_IRQ_BANK0` raw handler (`gpio_add_raw_irq_handler_
   masked64`) for DIO1. That CANNOT coexist with
   `gpio_set_irq_enabled_with_callback` (the GPS pump's CS-edge API). So:
   RadioLib + PicoHal run ENTIRELY on core1; core0's pump drives CS by polling
   and registers NO bank IRQ. Do not add a CS edge IRQ on core0, and do not
   call any RadioLib/PicoHal function from core0.
4. **core0 is the SINGLE WRITER of the outbound frame.** Only core0 calls
   `stage_frame`. core1 produces into the mailboxes (`s_tx_resp*`, `s_lis_resp*`,
   `s_rxind_*`) and core0 drains them. Do NOT stage from core1. **Staging
   priority (fixed in multi-check): SOLICITED responses first** — a ready
   `TX_RESP`/`LISTEN_RESP` the hub is awaiting is staged BEFORE an unsolicited
   `RX_IND`, so a busy listen window's RX stream can't starve the reply to a
   command. Keep that order in `spi_after_txn`.
5. **core0 NEVER touches spi1 or the radio.** PING_REQ `radios_ready` reads the
   cached `s_radio_ready` that core1 sets after `sx_init`.
6. **main.c is C and MUST NOT include any RadioLib C++ header.** The radio is
   reached ONLY through the `sx_*` extern-"C" wrappers in `sx_shim.cpp`.
7. **NEVER call tone()/noTone() (R-TONE).** PicoHal's `noTone()` calls
   `multicore_reset_core1()` — it would reset the radio core. RadioLib's SX1262
   path does not call them; keep it that way.
8. **RadioLib is upstream, PINNED to 7.8.1 (HARD), not vendored here.**
   `-DRADIOLIB_PATH` points at the user's checkout. `sx_shim.cpp` calls the
   POSITIONAL `SX1262::begin(freq,bw,sf,...)` — deprecated-but-present in 7.x,
   REMOVED on 8.x/master (→ `begin(const ConfigLoRa_t&)`). A `#error` guard in
   `sx_shim.cpp` fires on `RADIOLIB_VERSION_MAJOR > 7`, so 8.x fails loudly by
   design — port `begin()` to `ConfigLoRa_t` to upgrade, do not just bump the
   pin. Do not fork or patch RadioLib — all our code lives in `sx_shim.cpp` +
   `main.c`. PicoHal is RadioLib's own file, compiled as-is from
   `src/hal/RPiPico/PicoHal.cpp`.
9. **The RadioLib SX1262 call sequence is carried from the proven arduino
   blade** (begin with TCXO 1.6 V, transmit, startReceive, readData, getRSSI/
   getSNR, RF_SW1). Do not "improve" the RF config without a bench.
10. **LoRa TX is authorized-use-only, hub-gated, never auto.** Antenna attached
    before any TX.

## Known bench risks (see main.c KNOWN RISKS)
- **R-IRQBANK:** CS polling must keep up with the hub at 8 MHz / 261 B. The
  full-reset+preload per transaction (not the edge ISR) is what makes the GPS
  stream work and is preserved, so polling should suffice — but it is the first
  thing to check if the link slips. Fallback: a core0 PIO CS-realign (no bank
  IRQ), NOT done.
- **R-RLIB / R-LINK:** RadioLib API signatures + the mixed C/C++ link — confirm
  on the first build against your pinned RadioLib.
- **R-RXPOLL:** the listen window polls `getPacketLength()`; confirm a received
  frame latches it (the arduino blade used `checkDataReady()` first). If not,
  add the explicit IRQ-flag gate in `sx_rx_poll`.
- **R-RFSW:** RF_SW1 sense (HIGH=TX assumed) — verify on your Wio rev.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. "Compiles" = compile-correct-by-reading,
residual unknowns named, never claimed resolved. Docs travel with the edit.
