# CONTINUITY.md — remix-fh-pico-sdk-sx1262 (resume point)

Snapshot for picking this blade back up. Pair with `AGENTS.md` (constraints),
`WIRING.md` (hookup), `FUTURE.md`, and the KNOWN RISKS block in `main.c`.

## Where this blade stands (2026-11-01)
- **Pico-sdk port of the arduino sx1262 LoRa blade — first draft, complete.**
  Pulled AHEAD of nrf24 in the port order because LoRa is the critical-path
  comms link to the sensor stick (still being built). The committed order was
  cc1101 → nrf24 → sx1262; sx1262 was promoted for the dependency (recorded in
  `../remix-PICO-SDK-PORT-PLAN.md`).
- **Hub transport = the bench-proven GPS pump on spi0**, with the one sanctioned
  change: CS is POLLED, not an edge IRQ (R-IRQBANK).
- **Radio = RadioLib + its official Pico-SDK HAL (PicoHal) on spi1**, reached
  via `sx_shim.cpp`. This is the cc1101 philosophy applied to LoRa: reuse the
  vetted upstream library, write only the extern-"C" glue + verb mapping.

## Why RadioLib-on-SDK is viable (the key finding)
RadioLib ships an in-tree Pico-SDK HAL: `src/hal/RPiPico/PicoHal.{h,cpp}`, with
a NonArduino/Pico example. Construct `PicoHal(spi1, miso, mosi, sck)`, then
`new Module(hal, nss, dio1, rst, busy)`, then the normal RadioLib SX1262
`begin()/transmit()/startReceive()/readData()`. Confirmed from upstream source
(read, not assumed). Build: `add_subdirectory(RadioLib)` + define
`RADIOLIB_BUILD_RPI_PICO` + compile `PicoHal.cpp` + link `RadioLib`.

## Files in this package
- `main.c` — core0 hub pump (CS polled) + core1 LoRa executor; LoRa TX, listen
  window, unsolicited RX_IND; single-writer staging.
- `sx_shim.cpp` — extern-"C" bridge (`sx_init/sx_present/sx_tx/sx_listen/
  sx_rx_poll`) over RadioLib + PicoHal (spi1).
- `fh_pico_pins_sdk.h` — hub spi0 (GP18/16/19/21), radio spi1 (GP10/11/12 +
  NSS13/RST8/BUSY9/DIO1-7/RFSW1-6).
- `CMakeLists.txt` — needs `-DRADIOLIB_PATH=/path/to/RadioLib` (pin a tag).
- `fh_frame.c/.h`, `fh_ondemand.h`, `newlib_lock_stubs.c`, `uf2conv.py` —
  byte-identical copies from the GPS blade.
- `PICO_LESSONS.md` — verbatim GPS master (md5 `a39f5c37212e6e9c584f5e5dcf2e5511`).

## Design decisions locked in (do not revert)
- **R-IRQBANK:** RadioLib+PicoHal (incl. DIO1 bank IRQ) on core1; core0 pump
  polls CS, registers no bank IRQ. The one hard call — see AGENTS §3 + main.c.
- **R-TONE:** never call tone()/noTone() (PicoHal's noTone resets core1).
- SPI split inverted vs the arduino blade: hub spi0 (proven), radio spi1.
- core0 single writer; core1 produces into mailboxes.
- main.c (C) never includes a RadioLib header; reaches it via `sx_shim.cpp`.

## What is NOT done (bench + follow-ups)
- Nothing bench-proven. The RadioLib-on-SDK radio, the CS-polling pump under
  load, RF bring-up (TCXO 1.6 V, RF_SW1 sense), and the RX_IND latch
  (R-RXPOLL) are all unverified — see `main.c` KNOWN RISKS.
- ISM band is **915 MHz (North America)**, hardcoded in `sx_init()` — committed
  (the rig operates in North America; 868/EU is not a target).
- The arduino blade `../archive/arduino-blades/remix-fh-pico-sx1262/` stays until this is bench-proven.

## Next session should
1. With bench results: work the `WIRING.md` failure tree; watch R-IRQBANK
   (CS polling at speed) and R-RXPOLL (does getPacketLength latch) first.
2. nrf24 is still the remaining port (RF24 library; no obvious SDK drop-in —
   investigate the driver first, like we did for RadioLib here).
