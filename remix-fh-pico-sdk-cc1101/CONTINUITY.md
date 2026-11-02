# CONTINUITY.md — remix-fh-pico-sdk-cc1101 (resume point)

Snapshot for picking this blade back up after a drop. Pair with `AGENTS.md`
(constraints), `WIRING.md` (hookup), `FUTURE.md` (what's next), and the KNOWN
RISKS block at the end of `main.c`.

## Where this blade stands (2026-11-01)
- **Pilot pico-sdk port of the arduino cc1101 blade — COMPLETE as a first draft,
  reasoned-not-compiled, NOT bench-tested.** This is the first of the three
  committed blade ports (cc1101 → nrf24 → sx1262).
- The **hub-link pump is the bench-proven GPS blade pump, copied verbatim** onto
  **spi0** (slave, MODE 1). This is the trusted half.
- The **radio runs on spi1** (master, MODE 0) via the vendored+vetted
  SmartRC-CC1101 SDK driver. This is the bench surface.

## Files in this package
- `main.c` — core0 hub pump (copied from GPS) + core1 radio executor; the
  capture ring, decode, RAW_OOK dump stream, REPLAY, and ext-band CC TX (0x34).
- `cc_shim.cpp` — extern-"C" bridge (`cc_present/cc_init/cc_enter_rx/cc_rssi/
  cc_set_freq/cc_tx_raw`) + the OOK transmit bit-bang.
- `ELECHOUSE_CC1101_SRC_DRV.cpp/.h` — vendored vetted driver, 2 FH-EDITs
  (SPI_BUS→spi1, getCC1101 guard).
- `fh_pico_pins_sdk.h` — hub spi0 (GP18/16/19/21), radio spi1 (GP10/11/12/13 +
  GDO0 GP14).
- `fh_frame.c/.h`, `fh_ondemand.h`, `newlib_lock_stubs.c`, `uf2conv.py` —
  byte-identical copies from the GPS blade.
- `CMakeLists.txt` — lists `main.c fh_frame.c newlib_lock_stubs.c
  ELECHOUSE_CC1101_SRC_DRV.cpp cc_shim.cpp`, with `PICO_NO_PICOTOOL=1`.
- `PICO_LESSONS.md` — verbatim copy of the GPS master (md5
  `a39f5c37212e6e9c584f5e5dcf2e5511`).

## Wire protocol (frozen, byte-identical fh_frame/fh_ondemand)
- Hub drives Bus B MODE 1, 261-byte transactions, two per poll, response staged
  for the NEXT transaction.
- Verbs wired: `FH_TYPE_PING→PONG`, `FH_OD_PING_REQ→RESP`,
  `FH_OD_CC1101_REQ→RESP` (LISTEN/RX_ONE/RAW_OOK/REPLAY), `FH_OD_DUMP_IND`
  chunk stream for RAW_OOK, ext-band CC TX `0x34→0x35` (IMPLEMENTED), JAM/BRUTE
  `0x30/0x32` → `FH_OD_EXT_STUB`.

## Design decisions locked in (do not revert)
- **SPI split inverted vs the arduino blade:** hub on spi0 (proven pump), radio
  on spi1. (2026-11-01.)
- **core0 is the single writer;** core1 produces into mailboxes. (Fixed in the
  multi-check — a lost-update race when both cores staged.)
- **core0 never touches spi1;** PING_REQ reads the cached `s_radio_ready`.
- **main.c (C) does not include the C++ driver header;** reaches it via the
  shim only.
- **GDO0 capture IRQ enabled only during a capture window** (capture_start/stop),
  sharing the one GPIO-bank callback on core0.

## What is NOT done (bench + follow-ups)
- **Nothing on this blade is bench-proven.** The radio half (spi1 bring-up, GDO0
  capture, decode, RSSI, driver on silicon) is entirely unverified.
- R-LINK (mixed C/C++ link) and R-SPI2 (shared callback under a live capture)
  are first-build/bench unknowns — see `main.c` KNOWN RISKS.
- The arduino blade `../archive/arduino-blades/remix-fh-pico-cc1101/` stays until this is bench-proven,
  then retires (one flashable artifact per blade).

## Next session should
1. If the user has bench results: work the failure decision tree in `WIRING.md`.
2. If continuing the port programme: nrf24 is next (then sx1262, the RadioLib
   shim). Use THIS blade's structure as the template — the GPS pump copy, the
   single-writer mailbox, the vendored-driver edit contract.
